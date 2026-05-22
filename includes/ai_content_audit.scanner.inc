<?php

/**
 * @file
 * Scan orchestration and clustering for AI Content Audit.
 *
 * This is the heart of the audit scan: it inventories nodes, runs vector
 * similarity searches against the configured AI Search index, builds clusters
 * of semantically related nodes via union-find on the similarity edges, and
 * persists clusters + candidate rows for review.
 *
 * Loaded on demand from the module file (hook_cron, hook_ai_async_callbacks,
 * and the admin "Scan Now" form). Scoring, LLM, and action-execution concerns
 * have been split into sibling includes to keep this file focused on the
 * scan/cluster pipeline itself.
 *
 * Public entry points:
 *   - ai_content_audit_start_run()      — start an incremental scan run.
 *   - ai_content_audit_process_batch()  — process one batch of pending nids;
 *                                          triggers finalize when queue empties.
 *   - ai_content_audit_finalize_run()   — build clusters from edges, store,
 *                                          score, rank, then hand off to LLM
 *                                          summarize + auto-draft (in llm.inc).
 *   - ai_content_audit_active_run()     — currently collecting run, if any.
 *
 * Related files:
 *   - ai_content_audit.scoring.inc — canonical pick, age/traffic scoring,
 *                                     composite rank, ROT classification.
 *   - ai_content_audit.llm.inc     — cluster summaries, AI draft edits.
 *   - ai_content_audit.execute.inc — apply approved candidate actions.
 *   - ai_content_audit.tools.inc   — AI agent tool definitions + implementations.
 *   - ai_content_audit.admin.inc   — dashboard, runs, clusters, queue, settings.
 */

// ---------------------------------------------------------------------------
// Run lifecycle
// ---------------------------------------------------------------------------

/**
 * Start a new incremental scan run.
 *
 * Inventories all nodes, stores the nid queue on the run row, and sets
 * status to 'collecting'. Returns the run_id.
 */
function ai_content_audit_start_run($profile = 'default') {
  $config = config('ai_content_audit.settings');
  $bundle_filter_mode = $config->get('bundle_filter_mode') ?: 'exclude';
  $filtered_bundles = array_keys(array_filter($config->get('excluded_bundles') ?: []));

  $nodes = ai_content_audit_inventory_nodes($filtered_bundles, $bundle_filter_mode);
  $nids = array_keys($nodes);

  $run_id = ai_content_audit_run_id();

  db_insert('ai_content_audit_run')->fields([
    'run_id'           => $run_id,
    'profile'          => $profile,
    'status'           => 'collecting',
    'started'          => REQUEST_TIME,
    'finished'         => 0,
    'node_count'       => 0,
    'cluster_count'    => 0,
    'total_nodes'      => count($nids),
    'processed_nodes'  => 0,
    'pending_nids'     => serialize($nids),
  ])->execute();

  return $run_id;
}

/**
 * Process one batch of nodes for an in-progress run.
 *
 * Performs similarity searches and writes edges to ai_content_audit_edge.
 * When the queue is exhausted, triggers finalization.
 *
 * Returns array with keys: processed, total, finalized.
 */
function ai_content_audit_process_batch($run_id, $batch_size = 50) {
  $run = db_query(
    'SELECT * FROM {ai_content_audit_run} WHERE run_id = :rid',
    [':rid' => $run_id]
  )->fetchAssoc();

  if (!$run || $run['status'] !== 'collecting') {
    return ['processed' => 0, 'total' => 0, 'finalized' => FALSE];
  }

  $pending = $run['pending_nids'] ? unserialize($run['pending_nids']) : [];
  if (empty($pending)) {
    ai_content_audit_finalize_run($run_id);
    return [
      'processed' => (int) $run['processed_nodes'],
      'total'     => (int) $run['total_nodes'],
      'finalized' => TRUE,
    ];
  }

  $config = config('ai_content_audit.settings');
  $similarity_threshold = (float) ($config->get('similarity_threshold') ?: 0.75);
  $index_id = $config->get('search_api_index') ?: '';

  // Load metadata for all nodes in scope (needed to check membership).
  $bundle_filter_mode = $config->get('bundle_filter_mode') ?: 'exclude';
  $filtered_bundles = array_keys(array_filter($config->get('excluded_bundles') ?: []));
  $all_nodes = ai_content_audit_inventory_nodes($filtered_bundles, $bundle_filter_mode);

  $batch = array_splice($pending, 0, (int) $batch_size);

  foreach ($batch as $nid) {
    if (!isset($all_nodes[$nid])) {
      continue;
    }
    $node = $all_nodes[$nid];
    _ai_content_audit_search_edges($run_id, $nid, $node, $all_nodes, $similarity_threshold, $index_id);
  }

  $processed = (int) $run['processed_nodes'] + count($batch);
  $finalized = empty($pending);

  db_update('ai_content_audit_run')->fields([
    'processed_nodes' => $processed,
    'pending_nids'    => $finalized ? NULL : serialize($pending),
  ])->condition('run_id', $run_id)->execute();

  if ($finalized) {
    ai_content_audit_finalize_run($run_id);
  }

  return [
    'processed' => $processed,
    'total'     => (int) $run['total_nodes'],
    'finalized' => $finalized,
  ];
}

/**
 * Finalize a run: build clusters from edges, score, rank, trigger lifecycle.
 */
function ai_content_audit_finalize_run($run_id) {
  $run = db_query(
    'SELECT * FROM {ai_content_audit_run} WHERE run_id = :rid',
    [':rid' => $run_id]
  )->fetchAssoc();

  if (!$run) {
    return;
  }

  module_load_include('inc', 'ai_content_audit', 'includes/ai_content_audit.scoring');

  $config = config('ai_content_audit.settings');
  $profile = $run['profile'] ?: 'default';

  // Build clusters from stored edges.
  $clusters = _ai_content_audit_clusters_from_edges($run_id);

  // Fallback: if no edges (Search API AI unavailable), use title overlap.
  if (empty($clusters)) {
    $bundle_filter_mode = $config->get('bundle_filter_mode') ?: 'exclude';
    $filtered_bundles = array_keys(array_filter($config->get('excluded_bundles') ?: []));
    $all_nodes = ai_content_audit_inventory_nodes($filtered_bundles, $bundle_filter_mode);
    if (!empty($all_nodes)) {
      $clusters = ai_content_audit_fallback_clusters($all_nodes);
    }
  }

  $cluster_count = ai_content_audit_store_clusters($clusters, $run_id, $profile);

  ai_content_audit_score_traffic($run_id);
  ai_content_audit_rank_clusters($run_id);

  // Trigger lifecycle analysis for every clustered node.
  _ai_content_audit_trigger_lifecycle($run_id);

  // Submit LLM summary jobs via ai_async (one per cluster).
  $needs_llm = module_exists('ai') && ($config->get('llm_summaries_enabled') || $config->get('auto_draft_edits_enabled'));
  if ($needs_llm) {
    module_load_include('inc', 'ai_content_audit', 'includes/ai_content_audit.llm');
  }
  if (module_exists('ai') && $config->get('llm_summaries_enabled') && module_exists('ai_async')) {
    _ai_content_audit_submit_summary_jobs($run_id);
  }
  elseif (module_exists('ai') && $config->get('llm_summaries_enabled')) {
    ai_content_audit_summarize_clusters($run_id);
  }

  if (module_exists('ai') && $config->get('auto_draft_edits_enabled')) {
    ai_content_audit_auto_draft_cluster_edits($run_id);
  }

  // Delete edge rows for this run.
  db_delete('ai_content_audit_edge')->condition('run_id', $run_id)->execute();

  db_update('ai_content_audit_run')->fields([
    'status'        => 'complete',
    'finished'      => REQUEST_TIME,
    'node_count'    => (int) $run['total_nodes'],
    'cluster_count' => $cluster_count,
    'pending_nids'  => NULL,
  ])->condition('run_id', $run_id)->execute();
}

/**
 * Return the active (collecting) run, or NULL.
 */
function ai_content_audit_active_run() {
  return db_query(
    "SELECT * FROM {ai_content_audit_run} WHERE status = 'collecting' ORDER BY started DESC LIMIT 1"
  )->fetchAssoc() ?: NULL;
}

// ---------------------------------------------------------------------------
// Edge collection
// ---------------------------------------------------------------------------

/**
 * Run similarity search for one node and write edges to ai_content_audit_edge.
 */
function _ai_content_audit_search_edges($run_id, $nid, array $node, array $all_nodes, $threshold, $index_id) {
  if (!module_exists('ai_search')) {
    return;
  }
  module_load_include('inc', 'ai_search', 'includes/rag.tools');
  if (!function_exists('ai_search_execute_rag_search')) {
    return;
  }

  $index_ids = $index_id ? [$index_id] : [];

  $result = ai_search_execute_rag_search([
    'query'           => $node['title'],
    'index_ids'       => $index_ids,
    'top_k'           => 6,
    'score_threshold' => $threshold,
    'entity_types'    => ['node'],
  ]);

  if (empty($result['sources']) || !is_array($result['sources'])) {
    return;
  }

  foreach ($result['sources'] as $source) {
    $match_nid = !empty($source['entity_id']) ? (int) $source['entity_id'] : 0;
    $score = !empty($source['score']) ? (float) $source['score'] : 0.0;

    if (!$match_nid || $match_nid === $nid || !isset($all_nodes[$match_nid])) {
      continue;
    }
    if ($score < $threshold) {
      continue;
    }

    $nid_a = min($nid, $match_nid);
    $nid_b = max($nid, $match_nid);

    // Upsert: update score if a higher one arrives for the same pair.
    $existing = db_query(
      'SELECT eid, score FROM {ai_content_audit_edge} WHERE run_id = :rid AND nid_a = :a AND nid_b = :b',
      [':rid' => $run_id, ':a' => $nid_a, ':b' => $nid_b]
    )->fetchAssoc();

    if ($existing) {
      if ($score > (float) $existing['score']) {
        db_update('ai_content_audit_edge')
          ->fields(['score' => $score])
          ->condition('eid', $existing['eid'])
          ->execute();
      }
    }
    else {
      db_insert('ai_content_audit_edge')->fields([
        'run_id' => $run_id,
        'nid_a'  => $nid_a,
        'nid_b'  => $nid_b,
        'score'  => $score,
      ])->execute();
    }
  }
}

/**
 * Build cluster arrays from stored edges via union-find.
 */
function _ai_content_audit_clusters_from_edges($run_id) {
  $edges = db_query(
    'SELECT nid_a, nid_b, score FROM {ai_content_audit_edge} WHERE run_id = :rid',
    [':rid' => $run_id]
  )->fetchAll(PDO::FETCH_ASSOC);

  if (empty($edges)) {
    return [];
  }

  // Collect all nids.
  $nids = [];
  foreach ($edges as $edge) {
    $nids[$edge['nid_a']] = TRUE;
    $nids[$edge['nid_b']] = TRUE;
  }
  $nids = array_keys($nids);

  $parent = [];
  foreach ($nids as $nid) {
    $parent[$nid] = $nid;
  }
  $edge_scores = [];

  foreach ($edges as $edge) {
    $a = (int) $edge['nid_a'];
    $b = (int) $edge['nid_b'];
    $s = (float) $edge['score'];
    ai_content_audit_union($parent, $a, $b);
    $key = $a . '-' . $b;
    $edge_scores[$key] = max(isset($edge_scores[$key]) ? $edge_scores[$key] : 0.0, $s);
  }

  // Group by root.
  $groups = [];
  foreach ($nids as $nid) {
    $root = ai_content_audit_find($parent, $nid);
    $groups[$root][] = $nid;
  }

  $clusters = [];
  foreach ($groups as $root => $group_nids) {
    if (count($group_nids) < 2) {
      continue;
    }
    $scores = [];
    foreach ($group_nids as $a) {
      foreach ($group_nids as $b) {
        if ($a >= $b) continue;
        $key = $a . '-' . $b;
        if (isset($edge_scores[$key])) {
          $scores[] = $edge_scores[$key];
        }
      }
    }
    $avg_score = $scores ? array_sum($scores) / count($scores) : 0.5;
    $clusters[] = ['nids' => $group_nids, 'similarity_score' => $avg_score];
  }

  return $clusters;
}

// ---------------------------------------------------------------------------
// Lifecycle triggering
// ---------------------------------------------------------------------------

/**
 * Queue every node in any cluster through ai_content_lifecycle.
 */
function _ai_content_audit_trigger_lifecycle($run_id) {
  if (!module_exists('ai_content_lifecycle')) {
    return;
  }
  if (!function_exists('_ai_content_lifecycle_maybe_queue')) {
    module_load_include('module', 'ai_content_lifecycle');
  }
  if (!function_exists('_ai_content_lifecycle_maybe_queue')) {
    return;
  }

  $nids = db_query(
    'SELECT DISTINCT nid FROM {ai_content_audit_candidate} WHERE run_id = :rid',
    [':rid' => $run_id]
  )->fetchCol();

  foreach ($nids as $nid) {
    $node = node_load((int) $nid);
    if ($node) {
      _ai_content_lifecycle_maybe_queue($node);
    }
  }
}

// ---------------------------------------------------------------------------
// Async LLM summaries
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Inventory
// ---------------------------------------------------------------------------

/**
 * Pull all published nodes into a lightweight inventory array.
 */
function ai_content_audit_inventory_nodes(array $filtered_bundles = [], $bundle_filter_mode = 'exclude') {
  $query = db_select('node', 'n')
    ->fields('n', ['nid', 'title', 'type', 'created', 'changed', 'status'])
    ->condition('n.status', 1)
    ->orderBy('n.changed', 'DESC');

  if (!empty($filtered_bundles)) {
    $op = $bundle_filter_mode === 'include' ? 'IN' : 'NOT IN';
    $query->condition('n.type', $filtered_bundles, $op);
  }

  $result = $query->execute();
  $nodes = [];
  foreach ($result as $row) {
    $nodes[$row->nid] = [
      'nid'     => (int) $row->nid,
      'title'   => $row->title,
      'bundle'  => $row->type,
      'created' => (int) $row->created,
      'changed' => (int) $row->changed,
      'status'  => (int) $row->status,
    ];
  }
  return $nodes;
}

// ---------------------------------------------------------------------------
// Clustering helpers (kept for fallback and direct scan path)
// ---------------------------------------------------------------------------

/**
 * Build clusters via Search API AI (used for direct/manual full scan).
 */
function ai_content_audit_build_clusters(array $nodes, $threshold = 0.75, $index_id = '') {
  if (!module_exists('ai_search')) {
    return ai_content_audit_fallback_clusters($nodes);
  }
  module_load_include('inc', 'ai_search', 'includes/rag.tools');
  if (!function_exists('ai_search_execute_rag_search')) {
    return ai_content_audit_fallback_clusters($nodes);
  }

  $nids = array_keys($nodes);
  $parent = [];
  foreach ($nids as $nid) {
    $parent[$nid] = $nid;
  }
  $edge_scores = [];
  $index_ids = $index_id ? [$index_id] : [];

  foreach ($nodes as $nid => $node) {
    $query = $node['title'];
    $result = ai_search_execute_rag_search([
      'query'           => $query,
      'index_ids'       => $index_ids,
      'top_k'           => 6,
      'score_threshold' => $threshold,
      'entity_types'    => ['node'],
    ]);

    if (empty($result['sources']) || !is_array($result['sources'])) {
      continue;
    }

    foreach ($result['sources'] as $source) {
      $match_nid = !empty($source['entity_id']) ? (int) $source['entity_id'] : 0;
      $score = !empty($source['score']) ? (float) $source['score'] : 0.0;

      if (!$match_nid || $match_nid === $nid || !isset($nodes[$match_nid])) {
        continue;
      }
      if ($score >= $threshold) {
        ai_content_audit_union($parent, $nid, $match_nid);
        $key = min($nid, $match_nid) . '-' . max($nid, $match_nid);
        $edge_scores[$key] = max(isset($edge_scores[$key]) ? $edge_scores[$key] : 0.0, $score);
      }
    }
  }

  return ai_content_audit_groups_from_parent($nodes, $parent, $edge_scores);
}

/**
 * Simple title-overlap fallback clustering when Search API AI is unavailable.
 */
function ai_content_audit_fallback_clusters(array $nodes) {
  $clusters = [];
  $assigned = [];

  foreach ($nodes as $nid => $node) {
    if (isset($assigned[$nid])) {
      continue;
    }
    $words_a = ai_content_audit_title_words($node['title']);
    $group = [$nid];
    $assigned[$nid] = TRUE;

    foreach ($nodes as $other_nid => $other_node) {
      if ($other_nid === $nid || isset($assigned[$other_nid])) {
        continue;
      }
      $words_b = ai_content_audit_title_words($other_node['title']);
      $overlap = count(array_intersect($words_a, $words_b));
      $union = count(array_unique(array_merge($words_a, $words_b)));
      $jaccard = $union > 0 ? $overlap / $union : 0;

      if ($jaccard >= 0.4) {
        $group[] = $other_nid;
        $assigned[$other_nid] = TRUE;
      }
    }

    if (count($group) > 1) {
      $clusters[] = ['nids' => $group, 'similarity_score' => 0.5];
    }
  }

  return $clusters;
}

/**
 * Extract meaningful words from a title for simple overlap scoring.
 */
function ai_content_audit_title_words($title) {
  $stop = ['the', 'a', 'an', 'in', 'of', 'for', 'to', 'and', 'or', 'is', 'on', 'at', 'by', 'with', 'from'];
  $words = preg_split('/\W+/u', strtolower($title), -1, PREG_SPLIT_NO_EMPTY);
  return array_values(array_diff($words, $stop));
}

/**
 * Union-find: find root.
 */
function ai_content_audit_find(&$parent, $x) {
  if ($parent[$x] !== $x) {
    $parent[$x] = ai_content_audit_find($parent, $parent[$x]);
  }
  return $parent[$x];
}

/**
 * Union-find: merge two sets.
 */
function ai_content_audit_union(&$parent, $a, $b) {
  $ra = ai_content_audit_find($parent, $a);
  $rb = ai_content_audit_find($parent, $b);
  if ($ra !== $rb) {
    $parent[$rb] = $ra;
  }
}

/**
 * Build cluster arrays from union-find parent map.
 */
function ai_content_audit_groups_from_parent(array $nodes, array &$parent, array $edge_scores) {
  $groups = [];
  foreach (array_keys($nodes) as $nid) {
    $root = ai_content_audit_find($parent, $nid);
    $groups[$root][] = $nid;
  }

  $clusters = [];
  foreach ($groups as $root => $nids) {
    if (count($nids) < 2) {
      continue;
    }
    $scores = [];
    foreach ($nids as $a) {
      foreach ($nids as $b) {
        if ($a >= $b) continue;
        $key = $a . '-' . $b;
        if (isset($edge_scores[$key])) {
          $scores[] = $edge_scores[$key];
        }
      }
    }
    $avg_score = $scores ? array_sum($scores) / count($scores) : 0.5;
    $clusters[] = ['nids' => $nids, 'similarity_score' => $avg_score];
  }

  return $clusters;
}

// ---------------------------------------------------------------------------
// Store, score, rank
// ---------------------------------------------------------------------------

/**
 * Persist cluster and candidate rows. Returns count of clusters stored.
 */
function ai_content_audit_store_clusters(array $clusters, $run_id, $profile) {
  $nodes_cache = [];
  $count = 0;

  foreach ($clusters as $cluster_data) {
    $nids = $cluster_data['nids'];
    $sim_score = (float) $cluster_data['similarity_score'];

    $node_rows = ai_content_audit_load_node_rows($nids, $nodes_cache);
    $canonical_nid = ai_content_audit_pick_canonical($node_rows);
    $topic_label = !empty($node_rows[$canonical_nid]['title']) ? $node_rows[$canonical_nid]['title'] : 'Unknown cluster';
    $age_score = ai_content_audit_age_score($node_rows);

    $cid = db_insert('ai_content_audit_cluster')->fields([
      'run_id'             => $run_id,
      'profile'            => $profile,
      'topic_label'        => substr($topic_label, 0, 512),
      'canonical_nid'      => $canonical_nid,
      'source_count'       => count($nids),
      'similarity_score'   => $sim_score,
      'traffic_score'      => 0.0,
      'age_score'          => $age_score,
      'composite_score'    => 0.0,
      'review_state'       => 'pending',
      'rot_type'           => ai_content_audit_rot_classify($sim_score, $age_score),
      'recommended_action' => ai_content_audit_recommend_action($sim_score, $age_score),
      'created'            => REQUEST_TIME,
      'updated'            => REQUEST_TIME,
    ])->execute();

    foreach ($nids as $nid) {
      $is_canonical = $nid === $canonical_nid ? 1 : 0;
      $title = !empty($node_rows[$nid]['title']) ? $node_rows[$nid]['title'] : '';
      $bundle = !empty($node_rows[$nid]['type']) ? $node_rows[$nid]['type'] : '';

      db_insert('ai_content_audit_candidate')->fields([
        'cluster_id'     => $cid,
        'run_id'         => $run_id,
        'nid'            => $nid,
        'bundle'         => $bundle,
        'title'          => substr($title, 0, 512),
        'similarity_score' => $sim_score,
        'is_canonical'   => $is_canonical,
        'task_type'      => $is_canonical ? 'canonical' : 'duplicate_candidate',
        'status'         => 'pending',
        'created'        => REQUEST_TIME,
        'updated'        => REQUEST_TIME,
      ])->execute();
    }

    $count++;
  }

  return $count;
}

/**
 * Load node rows for a set of nids, using a simple cache array.
 */
function ai_content_audit_load_node_rows(array $nids, array &$cache) {
  $missing = array_diff($nids, array_keys($cache));
  if ($missing) {
    $result = db_query('SELECT nid, title, type, created, changed FROM {node} WHERE nid IN (:nids)', [':nids' => $missing]);
    foreach ($result as $row) {
      $cache[$row->nid] = (array) $row;
    }
  }
  $rows = [];
  foreach ($nids as $nid) {
    if (isset($cache[$nid])) {
      $rows[$nid] = $cache[$nid];
    }
  }
  return $rows;
}

// Cluster scoring (canonical pick, age, traffic, ROT, ranking) lives in
// ai_content_audit.scoring.inc.

// LLM-driven summaries and auto-draft edits live in ai_content_audit.llm.inc.

// Action execution lives in ai_content_audit.execute.inc.
