<?php

/**
 * @file
 * Cluster scoring and ROT classification.
 *
 * Extracted from ai_content_audit.scanner.inc. Pure functions over already-
 * built clusters: pick canonical, age score, traffic score, composite rank,
 * ROT classification, recommended action. No clustering or LLM here.
 */

module_load_include('inc', 'ai_content_audit', 'includes/ai_content_audit.fields');

/**
 * Similarity required before a related-content cluster becomes actionable.
 *
 * The scan threshold is intentionally lower so editors can discover related
 * pages. Action recommendations need stronger evidence than discovery alone.
 */
define('AI_CONTENT_AUDIT_ACTIONABLE_SIMILARITY_THRESHOLD', 0.85);

/**
 * Similarity required before an action can recommend pruning.
 */
define('AI_CONTENT_AUDIT_PRUNE_SIMILARITY_THRESHOLD', 0.90);

/**
 * Minimum creation age for a conservative legacy-content retire suggestion.
 */
define('AI_CONTENT_AUDIT_RETIRE_MIN_NODE_AGE_MONTHS', 120);

/**
 * Maximum normalized traffic score considered low enough for retirement.
 */
define('AI_CONTENT_AUDIT_RETIRE_MAX_TRAFFIC_SCORE', 0.05);

/**
 * Pick canonical node using multi-factor evaluation: traffic, length, and recency.
 *
 * Rather than selecting whatever duplicate happened to have a minor typo fixed
 * yesterday, this evaluates:
 * - Traffic (GA4 views): The page users visit and search engines index.
 * - Content substantiveness: Length/word count of the body (comprehensive page
 *   preferred over a stub).
 * - Recency & authority: Established creation and recent updates.
 *
 * @param array $node_rows
 *   Array of node rows keyed by nid (containing changed, created, title, type).
 * @param array $traffic_by_nid
 *   Optional preloaded traffic counts by nid.
 * @param array $length_by_nid
 *   Optional preloaded body lengths by nid.
 *
 * @return int
 *   The selected canonical NID.
 */
function ai_content_audit_pick_canonical(array $node_rows, array $traffic_by_nid = [], array $length_by_nid = []) {
  if (empty($node_rows)) {
    return 0;
  }
  if (count($node_rows) === 1) {
    return (int) key($node_rows);
  }

  $nids = array_keys($node_rows);

  // 1. Preload traffic if not provided.
  if (empty($traffic_by_nid) && db_table_exists('ai_content_audit_traffic')) {
    $traffic_count = db_query('SELECT COUNT(*) FROM {ai_content_audit_traffic}')->fetchField();
    if ($traffic_count) {
      $sources = array_map(function($nid) { return 'node/' . $nid; }, $nids);
      $aliases = db_query(
        'SELECT source, alias FROM {url_alias} WHERE source IN (:sources)',
        [':sources' => $sources]
      )->fetchAllKeyed();

      $nid_paths = [];
      foreach ($nids as $nid) {
        $nid_paths[$nid] = ['/node/' . $nid];
        $src = 'node/' . $nid;
        if (!empty($aliases[$src])) {
          $nid_paths[$nid][] = '/' . ltrim($aliases[$src], '/');
        }
      }
      $all_paths = array_values(array_unique(array_merge(...array_values($nid_paths))));
      $traffic_rows = db_query(
        'SELECT path, pageviews FROM {ai_content_audit_traffic} WHERE path IN (:paths)',
        [':paths' => $all_paths]
      )->fetchAllKeyed();

      foreach ($nids as $nid) {
        $views = 0;
        foreach ($nid_paths[$nid] as $path) {
          if (!empty($traffic_rows[$path])) {
            $views = max($views, (int) $traffic_rows[$path]);
          }
        }
        $traffic_by_nid[$nid] = $views;
      }
    }
  }

  // 2. Preload editable text lengths. This includes paragraph-backed content,
  // which is common for article bundles on this site.
  if (empty($length_by_nid)) {
    foreach ($nids as $nid) {
      $length_by_nid[$nid] = ai_content_audit_get_node_audit_text_length($nid);
    }
  }

  $max_traffic = !empty($traffic_by_nid) ? max($traffic_by_nid) : 0;
  $max_length = !empty($length_by_nid) ? max($length_by_nid) : 0;

  $min_changed = PHP_INT_MAX;
  $max_changed = 0;
  foreach ($node_rows as $row) {
    $c = (int) ($row['changed'] ?? 0);
    if ($c > 0) {
      $min_changed = min($min_changed, $c);
      $max_changed = max($max_changed, $c);
    }
  }
  $changed_range = max(1, $max_changed - $min_changed);

  $best = NULL;
  $best_score = -1.0;

  foreach ($node_rows as $nid => $row) {
    $traffic = (int) ($traffic_by_nid[$nid] ?? 0);
    $length = (int) ($length_by_nid[$nid] ?? 0);
    $changed = (int) ($row['changed'] ?? 0);

    $traffic_norm = ($max_traffic > 0) ? ($traffic / $max_traffic) : 0.0;
    $length_norm = ($max_length > 0) ? ($length / $max_length) : 0.0;
    $changed_norm = ($max_changed > $min_changed) ? (($changed - $min_changed) / $changed_range) : 0.5;

    // Weight allocation:
    // If traffic data exists across the cluster, it's a dominant signal (50%).
    // Content length prevents stubs from winning (35%).
    // Recency provides tie-breaking (15%).
    if ($max_traffic > 0) {
      $score = ($traffic_norm * 0.50) + ($length_norm * 0.35) + ($changed_norm * 0.15);
    }
    else {
      // Without traffic, substantiveness (65%) and recency/authority (35%) decide.
      $score = ($length_norm * 0.65) + ($changed_norm * 0.35);
    }

    if ($score > $best_score) {
      $best_score = $score;
      $best = $nid;
    }
    elseif (abs($score - $best_score) < 0.0001) {
      // Tie breaker: prefer most recently updated.
      if ($changed > (int) ($node_rows[$best]['changed'] ?? 0)) {
        $best = $nid;
      }
    }
  }

  return $best ? (int) $best : (int) key($node_rows);
}

/**
 * Calculate smooth age score for a single timestamp.
 *
 * 0.0 = fresh (below decay start), 1.0 = fully stale (above stale threshold).
 * Scales linearly between decay_start_months and stale_age_months.
 *
 * @param int $changed
 *   Unix timestamp of node changed time.
 * @param int|null $stale_age_months
 *   Months to reach 1.0 staleness (default 24).
 * @param int|null $decay_start_months
 *   Months before decay begins (default 6).
 *
 * @return float
 *   Score between 0.0 and 1.0.
 */
function ai_content_audit_node_age_score($changed, $stale_age_months = NULL, $decay_start_months = NULL) {
  $changed = (int) $changed;
  if (!$changed) {
    return 0.0;
  }

  if ($stale_age_months === NULL || $decay_start_months === NULL) {
    $config = config('ai_content_audit.settings');
    if ($stale_age_months === NULL) {
      $stale_age_months = (int) ($config->get('stale_age_months') ?: 24);
    }
    if ($decay_start_months === NULL) {
      $decay_start_months = (int) ($config->get('decay_start_months') ?: 6);
    }
  }

  $age_seconds = REQUEST_TIME - $changed;
  $min_seconds = max(0, $decay_start_months) * 30 * 86400;
  $max_seconds = max($decay_start_months + 1, $stale_age_months) * 30 * 86400;

  if ($age_seconds <= $min_seconds) {
    return 0.0;
  }
  if ($age_seconds >= $max_seconds) {
    return 1.0;
  }

  $score = ($age_seconds - $min_seconds) / ($max_seconds - $min_seconds);
  return round(min(1.0, max(0.0, $score)), 4);
}

/**
 * Score cluster age as the average smooth decay of all member nodes.
 *
 * 0.0 = all fresh, 1.0 = all stale.
 */
function ai_content_audit_age_score(array $node_rows, $stale_age_months = NULL, $decay_start_months = NULL) {
  if (empty($node_rows)) {
    return 0.0;
  }
  $scores = [];
  foreach ($node_rows as $row) {
    $changed = isset($row['changed']) ? (int) $row['changed'] : 0;
    $scores[] = ai_content_audit_node_age_score($changed, $stale_age_months, $decay_start_months);
  }
  return round(array_sum($scores) / count($scores), 4);
}

/**
 * Calculate the actual age of a node in 30-day months.
 *
 * This is a display/reporting value. Unlike age_score, it is not flattened to
 * 0 or 1 by freshness and staleness thresholds.
 */
function ai_content_audit_node_age_months($changed) {
  $changed = (int) $changed;
  if (!$changed || REQUEST_TIME <= $changed) {
    return 0.0;
  }

  return round((REQUEST_TIME - $changed) / (30 * 86400), 2);
}

/**
 * Calculate a node's actual age from its creation timestamp.
 *
 * This intentionally differs from ai_content_audit_node_age_months(), which
 * measures time since the last change for freshness/staleness context.
 */
function ai_content_audit_node_created_age_months($created) {
  $created = (int) $created;
  if (!$created || REQUEST_TIME <= $created) {
    return 0.0;
  }

  return round((REQUEST_TIME - $created) / (30 * 86400), 2);
}

/**
 * Classify a cluster using the ROT framework.
 *
 * - Redundant: near-identical content, both relatively recent.
 * - Outdated: strong overlap with stale members, or a later lifecycle signal.
 * - Trivial: loosely related, low composite priority.
 *
 * Related-content discovery below the actionable similarity threshold returns
 * 'review' regardless of age. This prevents old but distinct articles from
 * being labeled as refresh work.
 *
 * Returns a string: 'redundant', 'outdated', 'trivial',
 * 'redundant_outdated', or 'review'.
 */
function ai_content_audit_rot_classify($similarity_score, $age_score, $composite_score = 0.0) {
  $is_similar = $similarity_score >= AI_CONTENT_AUDIT_ACTIONABLE_SIMILARITY_THRESHOLD;
  $is_old     = $age_score >= 0.5;
  $is_trivial = $composite_score < 0.35 && !$is_similar;

  if ($is_trivial) {
    return 'trivial';
  }
  // A merely related and old cluster is still a review item. Age alone must
  // not turn distinct articles into refresh or consolidation work.
  if (!$is_similar) {
    return 'review';
  }
  if ($is_old) {
    return 'redundant_outdated';
  }
  return 'redundant';
}

/**
 * Derive recommended action from ROT classification.
 */
function ai_content_audit_recommend_action($similarity_score, $age_score, $composite_score = 0.0) {
  $rot = ai_content_audit_rot_classify($similarity_score, $age_score, $composite_score);
  switch ($rot) {
    case 'redundant':
      return $similarity_score >= AI_CONTENT_AUDIT_PRUNE_SIMILARITY_THRESHOLD ? 'prune' : 'merge';
    case 'redundant_outdated':
      return $similarity_score >= AI_CONTENT_AUDIT_PRUNE_SIMILARITY_THRESHOLD ? 'prune' : 'merge';
    case 'outdated':
      return 'refresh';
    case 'trivial':
      return 'review';
    default:
      return 'review';
  }
}

/**
 * Fold per-node lifecycle verdicts into cluster recommendations.
 *
 * The lifecycle analyzer marks a node review_status 'analyzed' when its LLM
 * pass decided the content needs updating (ai_results holds the reasoning).
 * Clusters that similarity/age alone left at 'review' are upgraded to
 * 'refresh' when the canonical node is flagged or at least half the members
 * are — which also makes them eligible for auto-drafted canonical edits,
 * whose prompt already cites the lifecycle analysis.
 *
 * @return int
 *   Number of clusters upgraded.
 */
function ai_content_audit_apply_lifecycle_signals($run_id) {
  if (!module_exists('ai_content_lifecycle') || !db_table_exists('content_lifecycle')) {
    return 0;
  }

  $rows = db_query(
    'SELECT ca.cluster_id, ca.nid, ca.is_canonical, c.recommended_action
       FROM {ai_content_audit_candidate} ca
      INNER JOIN {ai_content_audit_cluster} c ON c.cid = ca.cluster_id
      WHERE ca.run_id = :rid',
    [':rid' => $run_id]
  )->fetchAll(PDO::FETCH_ASSOC);
  if (empty($rows)) {
    return 0;
  }

  $nids = array_values(array_unique(array_map(function ($row) {
    return (int) $row['nid'];
  }, $rows)));
  $flagged_nids = db_query(
    "SELECT entity_id FROM {content_lifecycle} WHERE entity_type = 'node' AND review_status = 'analyzed' AND entity_id IN (:nids)",
    [':nids' => $nids]
  )->fetchCol();
  if (empty($flagged_nids)) {
    return 0;
  }
  $flagged = array_fill_keys(array_map('intval', $flagged_nids), TRUE);

  $clusters = [];
  foreach ($rows as $row) {
    $cid = (int) $row['cluster_id'];
    if (!isset($clusters[$cid])) {
      $clusters[$cid] = [
        'action' => (string) $row['recommended_action'],
        'members' => 0,
        'flagged' => 0,
        'canonical_flagged' => FALSE,
      ];
    }
    $clusters[$cid]['members']++;
    if (!empty($flagged[(int) $row['nid']])) {
      $clusters[$cid]['flagged']++;
      if (!empty($row['is_canonical'])) {
        $clusters[$cid]['canonical_flagged'] = TRUE;
      }
    }
  }

  $upgraded = 0;
  foreach ($clusters as $cid => $info) {
    if ($info['action'] !== 'review' || !$info['flagged']) {
      continue;
    }
    if ($info['canonical_flagged'] || $info['flagged'] * 2 >= $info['members']) {
      db_update('ai_content_audit_cluster')
        ->fields([
          'recommended_action' => 'refresh',
          'rot_type' => 'outdated',
          'updated' => REQUEST_TIME,
        ])
        ->condition('cid', $cid)
        ->execute();
      $upgraded++;
    }
  }

  if ($upgraded) {
    watchdog('ai_content_audit', 'Lifecycle signals upgraded @count cluster(s) to refresh for run @rid.', [
      '@count' => $upgraded,
      '@rid' => $run_id,
    ]);
  }
  return $upgraded;
}

/**
 * Score clusters by traffic using uploaded GA4 data.
 */
function ai_content_audit_score_traffic($run_id) {
  if (!db_table_exists('ai_content_audit_traffic')) {
    return;
  }
  $traffic_count = db_query('SELECT COUNT(*) FROM {ai_content_audit_traffic}')->fetchField();
  if (!$traffic_count) {
    return;
  }

  $cluster_ids = db_query(
    'SELECT cid FROM {ai_content_audit_cluster} WHERE run_id = :rid',
    [':rid' => $run_id]
  )->fetchCol();
  if (empty($cluster_ids)) {
    return;
  }

  $candidates = db_query(
    'SELECT cluster_id, nid FROM {ai_content_audit_candidate} WHERE cluster_id IN (:cids)',
    [':cids' => $cluster_ids]
  )->fetchAll(PDO::FETCH_ASSOC);
  if (empty($candidates)) {
    return;
  }

  $nids = array_values(array_unique(array_column($candidates, 'nid')));
  $sources = array_map(function($nid) { return 'node/' . $nid; }, $nids);
  $aliases = db_query(
    'SELECT source, alias FROM {url_alias} WHERE source IN (:sources)',
    [':sources' => $sources]
  )->fetchAllKeyed();

  $nid_paths = [];
  foreach ($nids as $nid) {
    $nid_paths[$nid] = ['/node/' . $nid];
    $src = 'node/' . $nid;
    if (!empty($aliases[$src])) {
      $nid_paths[$nid][] = '/' . ltrim($aliases[$src], '/');
    }
  }

  $all_paths = array_values(array_unique(array_merge(...array_values($nid_paths))));
  $traffic = db_query(
    'SELECT path, pageviews FROM {ai_content_audit_traffic} WHERE path IN (:paths)',
    [':paths' => $all_paths]
  )->fetchAllKeyed();

  $nid_views = [];
  foreach ($nids as $nid) {
    $views = 0;
    foreach ($nid_paths[$nid] as $path) {
      if (!empty($traffic[$path])) {
        $views = max($views, (int) $traffic[$path]);
      }
    }
    $nid_views[$nid] = $views;
  }

  $cluster_buckets = [];
  foreach ($candidates as $row) {
    $cluster_buckets[$row['cluster_id']][] = $nid_views[$row['nid']] ?? 0;
  }
  $cluster_avg = [];
  foreach ($cluster_buckets as $cid => $views_list) {
    $cluster_avg[$cid] = array_sum($views_list) / count($views_list);
  }

  $max = max($cluster_avg);
  if ($max <= 0) {
    return;
  }

  foreach ($cluster_avg as $cid => $avg) {
    db_update('ai_content_audit_cluster')
      ->fields(['traffic_score' => round($avg / $max, 4)])
      ->condition('cid', $cid)
      ->execute();
  }
}

/**
 * Retrieve normalized weights for similarity, age, and traffic.
 *
 * If GA4 traffic data is missing, the weights automatically re-normalize
 * across similarity and age so the maximum consolidation priority remains 1.0.
 *
 * @param bool|null $has_traffic
 *   Whether traffic data is present. If NULL, auto-detected from DB.
 *
 * @return array
 *   Associative array with keys 'similarity', 'age', 'traffic', and 'raw'.
 */
function ai_content_audit_get_priority_weights($has_traffic = NULL) {
  $config = config('ai_content_audit.settings');
  $w_sim = max(0, (float) ($config->get('weight_similarity') ?? 50));
  $w_age = max(0, (float) ($config->get('weight_age') ?? 30));
  $w_traf = max(0, (float) ($config->get('weight_traffic') ?? 20));

  if ($has_traffic === NULL) {
    $has_traffic = FALSE;
    if (db_table_exists('ai_content_audit_traffic')) {
      $has_traffic = (bool) db_query('SELECT COUNT(*) FROM {ai_content_audit_traffic}')->fetchField();
    }
  }

  if (!$has_traffic) {
    $sum = $w_sim + $w_age;
    $norm_sim = $sum > 0 ? ($w_sim / $sum) : 0.625;
    $norm_age = $sum > 0 ? ($w_age / $sum) : 0.375;
    $norm_traf = 0.0;
  }
  else {
    $sum = $w_sim + $w_age + $w_traf;
    $norm_sim = $sum > 0 ? ($w_sim / $sum) : 0.50;
    $norm_age = $sum > 0 ? ($w_age / $sum) : 0.30;
    $norm_traf = $sum > 0 ? ($w_traf / $sum) : 0.20;
  }

  return [
    'similarity' => $norm_sim,
    'age' => $norm_age,
    'traffic' => $norm_traf,
    'raw' => [
      'similarity' => $w_sim,
      'age' => $w_age,
      'traffic' => $w_traf,
    ],
    'has_traffic' => (bool) $has_traffic,
  ];
}

/**
 * Store a ROT assessment for every node in a scan.
 *
 * Exact duplicate membership and semantic similarity are supplied separately
 * from the per-node ROT signals. Similarity can identify redundant pages for
 * review, but only exact fingerprints create the separate duplicate workflow.
 * $merge_into maps near-duplicate nids a decision model confirmed as
 * interchangeable to the page they should merge into.
 */
function ai_content_audit_store_node_scores($run_id, array $nodes, array $duplicate_nids = [], array $similarity_by_nid = [], array $merge_into = []) {
  if (empty($nodes)) {
    return 0;
  }

  db_delete('ai_content_audit_node_score')->condition('run_id', $run_id)->execute();

  $nids = array_keys($nodes);
  $views = ai_content_audit_get_node_pageviews($nids);
  $max_views = !empty($views) ? max($views) : 0;
  $duplicate_map = array_fill_keys(array_map('intval', $duplicate_nids), TRUE);
  $weights = ai_content_audit_get_priority_weights($max_views > 0);
  $stored = 0;

  foreach ($nodes as $nid => $node) {
    $nid = (int) $nid;
    $age_months = ai_content_audit_node_age_months($node['changed']);
    $node_age_months = ai_content_audit_node_created_age_months($node['created']);
    $age_score = ai_content_audit_node_age_score($node['changed']);
    $traffic_score = $max_views > 0 ? round(($views[$nid] ?? 0) / $max_views, 4) : 0.0;
    $is_duplicate = isset($duplicate_map[$nid]);
    $similarity_score = $is_duplicate
      ? 1.0
      : max(0.0, min(1.0, (float) ($similarity_by_nid[$nid] ?? 0.0)));
    $content_length = ai_content_audit_get_node_audit_text_length($nid);

    // Priority uses the configured similarity, age, and traffic weights. A
    // missing traffic import re-normalizes the other two weights. Content
    // length remains a classification signal, not a substitute for similarity.
    $composite = round(
      ($similarity_score * $weights['similarity']) +
      ($age_score * $weights['age']) +
      ($traffic_score * $weights['traffic']),
      4
    );

    $rot_type = ai_content_audit_node_rot_classify($similarity_score, $age_score, $content_length);
    $recommended_action = ai_content_audit_node_recommend_action(
      $rot_type,
      $is_duplicate,
      $similarity_score,
      $age_score,
      $node_age_months,
      $traffic_score,
      (int) ($merge_into[$nid] ?? 0)
    );

    db_insert('ai_content_audit_node_score')->fields([
      'run_id' => $run_id,
      'nid' => $nid,
      'title' => substr($node['title'], 0, 512),
      'bundle' => substr($node['bundle'], 0, 128),
      'similarity_score' => $similarity_score,
      'age_months' => $age_months,
      'node_age_months' => $node_age_months,
      'age_score' => $age_score,
      'traffic_score' => $traffic_score,
      'composite_score' => $composite,
      'rot_type' => $rot_type,
      'recommended_action' => $recommended_action,
      'merge_into_nid' => $recommended_action === 'merge' ? (int) ($merge_into[$nid] ?? 0) : 0,
      'review_state' => 'pending',
      'created' => REQUEST_TIME,
      'updated' => REQUEST_TIME,
    ])->execute();
    $stored++;
  }

  return $stored;
}

/**
 * Classify one scanned page using independent ROT evidence.
 *
 * Similarity is required for redundancy. Age alone may identify outdated
 * content, but it must not turn a fresh, substantial page into a trivial item.
 * Exact duplicates arrive with similarity 1.0 from the scanner.
 */
function ai_content_audit_node_rot_classify($similarity_score, $age_score, $content_length) {
  $is_similar = (float) $similarity_score >= AI_CONTENT_AUDIT_ACTIONABLE_SIMILARITY_THRESHOLD;
  $is_old = (float) $age_score >= 0.5;

  if ($is_similar) {
    return $is_old ? 'redundant_outdated' : 'redundant';
  }
  if ($is_old) {
    return 'outdated';
  }
  if ((int) $content_length < 300) {
    return 'trivial';
  }
  return 'review';
}

/**
 * Recommend a human-reviewed next step for one ROT page.
 *
 * Retire is intentionally narrow: only legacy content that has reached the
 * full staleness threshold and has very low traffic receives it. It is a
 * recommendation, not an automatic unpublish action.
 */
function ai_content_audit_node_recommend_action($rot_type, $is_duplicate, $similarity_score, $age_score, $node_age_months = 0.0, $traffic_score = 0.0, $merge_into_nid = 0) {
  switch ($rot_type) {
    case 'redundant':
    case 'redundant_outdated':
      // Semantic similarity can classify a page as redundant, but only an
      // exact fingerprint belongs to the consolidation workflow. Keep
      // near-duplicate recommendations human-reviewed unless a decision
      // model confirmed the pages are interchangeable; then only the weaker
      // page of the pair (the one with a merge target) is recommended to merge.
      if ($is_duplicate) {
        return $similarity_score >= AI_CONTENT_AUDIT_PRUNE_SIMILARITY_THRESHOLD ? 'prune' : 'merge';
      }
      return $merge_into_nid > 0 ? 'merge' : 'review';
    case 'outdated':
      if ((float) $age_score >= 1.0
        && (float) $node_age_months >= AI_CONTENT_AUDIT_RETIRE_MIN_NODE_AGE_MONTHS
        && (float) $traffic_score <= AI_CONTENT_AUDIT_RETIRE_MAX_TRAFFIC_SCORE) {
        return 'retire';
      }
      return 'refresh';
    case 'trivial':
      return (float) $age_score >= 1.0
        && (float) $node_age_months >= AI_CONTENT_AUDIT_RETIRE_MIN_NODE_AGE_MONTHS
        && (float) $traffic_score <= AI_CONTENT_AUDIT_RETIRE_MAX_TRAFFIC_SCORE
        ? 'retire'
        : 'review';
    default:
      return 'review';
  }
}

/**
 * Return imported pageviews keyed by node ID.
 */
function ai_content_audit_get_node_pageviews(array $nids) {
  $views = array_fill_keys(array_map('intval', $nids), 0);
  if (empty($nids) || !db_table_exists('ai_content_audit_traffic')) {
    return $views;
  }
  if (!db_query('SELECT COUNT(*) FROM {ai_content_audit_traffic}')->fetchField()) {
    return $views;
  }

  $sources = array_map(function ($nid) { return 'node/' . $nid; }, $nids);
  $aliases = db_query(
    'SELECT source, alias FROM {url_alias} WHERE source IN (:sources)',
    [':sources' => $sources]
  )->fetchAllKeyed();
  $paths = [];
  foreach ($nids as $nid) {
    $paths[(int) $nid] = ['/node/' . $nid];
    $source = 'node/' . $nid;
    if (!empty($aliases[$source])) {
      $paths[(int) $nid][] = '/' . ltrim($aliases[$source], '/');
    }
  }
  $all_paths = array_values(array_unique(array_merge(...array_values($paths))));
  $traffic = db_query(
    'SELECT path, pageviews FROM {ai_content_audit_traffic} WHERE path IN (:paths)',
    [':paths' => $all_paths]
  )->fetchAllKeyed();
  foreach ($paths as $nid => $node_paths) {
    foreach ($node_paths as $path) {
      if (!empty($traffic[$path])) {
        $views[$nid] = max($views[$nid], (int) $traffic[$path]);
      }
    }
  }
  return $views;
}

/**
 * Update composite scores for clusters in a run.
 */
function ai_content_audit_rank_clusters($run_id) {
  $weights = ai_content_audit_get_priority_weights();

  $result = db_query(
    'SELECT cid, similarity_score, traffic_score, age_score FROM {ai_content_audit_cluster} WHERE run_id = :rid',
    [':rid' => $run_id]
  );
  foreach ($result as $row) {
    $composite = round(
      ($row->similarity_score * $weights['similarity']) +
      ($row->age_score * $weights['age']) +
      ($row->traffic_score * $weights['traffic']),
      4
    );
    // Priority is useful for ordering related-content review, but it does not
    // establish a canonical page or prove that members are duplicates. ROT
    // classification and consolidation targets require editorial evidence.
    db_update('ai_content_audit_cluster')
      ->fields([
        'composite_score' => $composite,
      ])
      ->condition('cid', $row->cid)
      ->execute();
  }
}
