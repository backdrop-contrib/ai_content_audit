<?php

/**
 * @file
 * AI agent tool definitions and execute callbacks for AI Content Audit.
 *
 * Exposes a set of structured tools an AI agent (via the AI Agents module)
 * can call to inspect audit state and stage actions for admin approval. All
 * write tools are gated — the agent stages an action; nothing mutates a node
 * until an admin approves it from the queue UI.
 *
 * Tools (registered via hook_ai_tools in the module file):
 *   - audit_list_clusters     — list clusters from the latest run.
 *   - audit_get_cluster       — full cluster detail incl. candidate scores.
 *   - audit_list_runs         — recent scan runs.
 *   - audit_trigger_scan      — kick off a scan batch.
 *   - audit_propose_action    — propose prune/merge/flag for one candidate.
 *   - audit_get_node_content  — read current node fields + lifecycle analysis,
 *                                surface the primary editable target context.
 *   - audit_propose_edit      — save proposed text as a draft revision; for
 *                                paragraph-backed bodies, writes a new
 *                                paragraph revision and stashes the pointers
 *                                in before_state for later unwind on reject.
 *
 * Field-level helpers (set_text_field_value, set_paragraph_target_value,
 * pick_primary_edit_target, normalize_text_for_compare, etc.) live in
 * ai_content_audit.fields.inc and are loaded here at the top of the file so
 * any execute callback in this file can use them without further loads.
 */

module_load_include('inc', 'ai_content_audit', 'includes/ai_content_audit.fields');

/**
 * Returns tool definitions for content audit operations.
 */
function ai_content_audit_ai_tools_definitions() {
  return [
    'audit_list_clusters' => [
      'definition' => [
        'type' => 'function',
        'function' => [
          'name' => 'audit_list_clusters',
          'description' => 'List content audit clusters from the most recent run, ordered by composite score descending. Returns cluster id, topic, recommended action, scores, and source count.',
          'parameters' => [
            'type' => 'object',
            'properties' => [
              'run_id' => [
                'type' => 'string',
                'description' => 'Specific run ID to filter by. Leave empty for all clusters from the latest run.',
              ],
              'review_state' => [
                'type' => 'string',
                'description' => 'Filter by review state: pending, dismissed, actioned.',
              ],
              'limit' => [
                'type' => 'integer',
                'description' => 'Maximum number of clusters to return (default 10, max 50).',
              ],
            ],
            'required' => [],
          ],
        ],
      ],
      'execute' => 'ai_content_audit_execute_list_clusters',
      'module' => 'ai_content_audit',
      'category' => 'content_audit',
      'operation' => 'read',
      'permission' => 'administer ai content audit',
    ],

    'audit_get_cluster' => [
      'definition' => [
        'type' => 'function',
        'function' => [
          'name' => 'audit_get_cluster',
          'description' => 'Get full details for a content cluster including all candidate nodes, scores, and the LLM summary.',
          'parameters' => [
            'type' => 'object',
            'properties' => [
              'cluster_id' => [
                'type' => 'integer',
                'description' => 'The cluster CID.',
              ],
            ],
            'required' => ['cluster_id'],
          ],
        ],
      ],
      'execute' => 'ai_content_audit_execute_get_cluster',
      'module' => 'ai_content_audit',
      'category' => 'content_audit',
      'operation' => 'read',
      'permission' => 'administer ai content audit',
    ],

    'audit_list_runs' => [
      'definition' => [
        'type' => 'function',
        'function' => [
          'name' => 'audit_list_runs',
          'description' => 'List recent content audit scan runs with their status, node count, and cluster count.',
          'parameters' => [
            'type' => 'object',
            'properties' => [
              'limit' => [
                'type' => 'integer',
                'description' => 'Maximum runs to return (default 10).',
              ],
            ],
            'required' => [],
          ],
        ],
      ],
      'execute' => 'ai_content_audit_execute_list_runs',
      'module' => 'ai_content_audit',
      'category' => 'content_audit',
      'operation' => 'read',
      'permission' => 'administer ai content audit',
    ],

    'audit_trigger_scan' => [
      'definition' => [
        'type' => 'function',
        'function' => [
          'name' => 'audit_trigger_scan',
          'description' => 'Trigger a content audit scan immediately. Returns the run_id and initial results.',
          'parameters' => [
            'type' => 'object',
            'properties' => [
              'batch_size' => [
                'type' => 'integer',
                'description' => 'Number of nodes to include in this scan (default 50, max 200).',
              ],
              'profile' => [
                'type' => 'string',
                'description' => 'Audit profile name. Defaults to "default".',
              ],
            ],
            'required' => [],
          ],
        ],
      ],
      'execute' => 'ai_content_audit_execute_trigger_scan',
      'module' => 'ai_content_audit',
      'category' => 'content_audit',
      'operation' => 'write',
      'permission' => 'administer ai content audit',
    ],

    'audit_propose_action' => [
      'definition' => [
        'type' => 'function',
        'function' => [
          'name' => 'audit_propose_action',
          'description' => 'Propose an action for a specific candidate node within a cluster. Action must be one of: prune, flag_for_review, dismiss. Does not execute — requires admin approval.',
          'parameters' => [
            'type' => 'object',
            'properties' => [
              'acid' => [
                'type' => 'integer',
                'description' => 'The candidate ACID (from audit_get_cluster results).',
              ],
              'action' => [
                'type' => 'string',
                'description' => 'Action to propose: prune (unpublish), merge (consolidate into canonical), review (flag for human review), edit_queued (admin will manually edit), ignored (no action needed), dismiss.',
                'enum' => ['prune', 'flag_for_review', 'merge', 'review', 'edit_queued', 'ignored', 'dismiss'],
              ],
              'reason' => [
                'type' => 'string',
                'description' => 'Brief explanation of why this action is recommended.',
              ],
            ],
            'required' => ['acid', 'action', 'reason'],
          ],
        ],
      ],
      'execute' => 'ai_content_audit_execute_propose_action',
      'module' => 'ai_content_audit',
      'category' => 'content_audit',
      'operation' => 'write',
      'permission' => 'administer ai content audit',
    ],

    'audit_get_node_content' => [
      'definition' => [
        'type' => 'function',
        'function' => [
          'name' => 'audit_get_node_content',
          'description' => 'Read the current title, text fields, and ai_content_lifecycle analysis for a node. The lifecycle.ai_analysis field contains AI-generated reasons why the content may be outdated or problematic — use this context when drafting proposed edits or classifying ROT.',
          'parameters' => [
            'type' => 'object',
            'properties' => [
              'nid' => [
                'type' => 'integer',
                'description' => 'The node ID to read.',
              ],
            ],
            'required' => ['nid'],
          ],
        ],
      ],
      'execute' => 'ai_content_audit_execute_get_node_content',
      'module' => 'ai_content_audit',
      'category' => 'content_audit',
      'operation' => 'read',
      'permission' => 'administer ai content audit',
    ],

    'audit_propose_edit' => [
      'definition' => [
        'type' => 'function',
        'function' => [
          'name' => 'audit_propose_edit',
          'description' => 'Save proposed text edits to a node as a draft revision so an admin can review the diff and approve or reject. Call audit_get_node_content first to read current content and lifecycle analysis. Use the lifecycle.ai_analysis to understand what specifically is outdated or problematic, and address those issues in your proposed edits.',
          'parameters' => [
            'type' => 'object',
            'properties' => [
              'nid' => [
                'type' => 'integer',
                'description' => 'The node ID to edit.',
              ],
              'acid' => [
                'type' => 'integer',
                'description' => 'The audit candidate ACID to link this proposed edit to. Optional but recommended.',
              ],
              'proposed_title' => [
                'type' => 'string',
                'description' => 'Proposed new title. Omit to keep existing title.',
              ],
              'proposed_body' => [
                'type' => 'string',
                'description' => 'Proposed new body text. Omit to keep existing body.',
              ],
              'proposed_summary' => [
                'type' => 'string',
                'description' => 'Proposed new body summary. Omit to keep existing summary.',
              ],
              'reason' => [
                'type' => 'string',
                'description' => 'Brief explanation of what was changed and why.',
              ],
            ],
            'required' => ['nid', 'reason'],
          ],
        ],
      ],
      'execute' => 'ai_content_audit_execute_propose_edit',
      'module' => 'ai_content_audit',
      'category' => 'content_audit',
      'operation' => 'write',
      'permission' => 'administer ai content audit',
    ],

    'audit_classify_rot' => [
      'definition' => [
        'type' => 'function',
        'function' => [
          'name' => 'audit_classify_rot',
          'description' => 'Set the ROT (Redundant, Outdated, Trivial) classification for a cluster after reviewing its content. Use audit_get_node_content to read pages before classifying. The heuristic pre-fills rot_type from scores alone — use this tool to correct it based on actual content.',
          'parameters' => [
            'type' => 'object',
            'properties' => [
              'cluster_id' => [
                'type' => 'integer',
                'description' => 'The cluster CID to classify.',
              ],
              'rot_type' => [
                'type' => 'string',
                'description' => 'ROT classification: redundant (near-identical, both recent), outdated (same topic but years apart — old page needs updating or archiving), trivial (low value, loosely related), redundant_outdated (duplicate AND stale), review (unclear — needs human).',
                'enum' => ['redundant', 'outdated', 'trivial', 'redundant_outdated', 'review'],
              ],
              'reason' => [
                'type' => 'string',
                'description' => 'Brief explanation of why this ROT classification was chosen.',
              ],
            ],
            'required' => ['cluster_id', 'rot_type'],
          ],
        ],
      ],
      'execute' => 'ai_content_audit_execute_classify_rot',
      'module' => 'ai_content_audit',
      'category' => 'content_audit',
      'operation' => 'write',
      'permission' => 'administer ai content audit',
    ],

    'audit_dismiss_cluster' => [
      'definition' => [
        'type' => 'function',
        'function' => [
          'name' => 'audit_dismiss_cluster',
          'description' => 'Mark a cluster as dismissed — the overlap is intentional or not actionable.',
          'parameters' => [
            'type' => 'object',
            'properties' => [
              'cluster_id' => [
                'type' => 'integer',
                'description' => 'The cluster CID to dismiss.',
              ],
              'reason' => [
                'type' => 'string',
                'description' => 'Why this cluster is being dismissed.',
              ],
            ],
            'required' => ['cluster_id'],
          ],
        ],
      ],
      'execute' => 'ai_content_audit_execute_dismiss_cluster',
      'module' => 'ai_content_audit',
      'category' => 'content_audit',
      'operation' => 'write',
      'permission' => 'administer ai content audit',
    ],
  ];
}

/**
 * Execute: list clusters.
 */
function ai_content_audit_execute_list_clusters(array $args) {
  $limit = min(50, max(1, (int) (!empty($args['limit']) ? $args['limit'] : 10)));
  $query = db_select('ai_content_audit_cluster', 'c')
    ->fields('c')
    ->orderBy('c.composite_score', 'DESC')
    ->range(0, $limit);

  if (!empty($args['run_id'])) {
    $query->condition('c.run_id', $args['run_id']);
  }
  else {
    $latest = db_query('SELECT run_id FROM {ai_content_audit_run} WHERE status = :s ORDER BY started DESC LIMIT 1', [':s' => 'complete'])->fetchField();
    if ($latest) {
      $query->condition('c.run_id', $latest);
    }
  }

  if (!empty($args['review_state'])) {
    $query->condition('c.review_state', $args['review_state']);
  }

  $rows = $query->execute()->fetchAllAssoc('cid', PDO::FETCH_ASSOC);
  if (empty($rows)) {
    return ['clusters' => [], 'message' => 'No clusters found. Run audit_trigger_scan first.'];
  }

  $output = [];
  foreach ($rows as $row) {
    $output[] = [
      'cluster_id' => (int) $row['cid'],
      'topic_label' => $row['topic_label'],
      'source_count' => (int) $row['source_count'],
      'similarity_score' => round((float) $row['similarity_score'], 3),
      'age_score' => round((float) $row['age_score'], 3),
      'composite_score' => round((float) $row['composite_score'], 3),
      'recommended_action' => $row['recommended_action'],
      'review_state' => $row['review_state'],
      'run_id' => $row['run_id'],
    ];
  }

  return ['clusters' => $output, 'count' => count($output)];
}

/**
 * Execute: get cluster detail.
 */
function ai_content_audit_execute_get_cluster(array $args) {
  $cid = (int) (!empty($args['cluster_id']) ? $args['cluster_id'] : 0);
  if (!$cid) {
    return ['error' => 'cluster_id required'];
  }

  $cluster = ai_content_audit_load_cluster($cid);
  if (!$cluster) {
    return ['error' => "Cluster $cid not found."];
  }

  $candidates = ai_content_audit_load_candidates($cid);

  // Bulk-load lifecycle data for all nids in this cluster.
  $all_nids = array_column($candidates, 'nid');
  $lifecycle_map = [];
  if (!empty($all_nids) && module_exists('ai_content_lifecycle') && db_table_exists('content_lifecycle')) {
    $rows = db_query(
      'SELECT entity_id, review_status, ai_results, changed FROM {content_lifecycle} WHERE entity_type = :type AND entity_id IN (:nids)',
      [':type' => 'node', ':nids' => $all_nids]
    );
    foreach ($rows as $row) {
      $lifecycle_map[(int) $row->entity_id] = [
        'review_status' => $row->review_status,
        'ai_analysis'   => $row->ai_results,
        'analyzed_date' => $row->changed ? date('Y-m-d', $row->changed) : '',
      ];
    }
  }

  $candidate_list = [];
  foreach ($candidates as $c) {
    $nid = (int) $c['nid'];
    $candidate_list[] = [
      'acid'             => (int) $c['acid'],
      'nid'              => $nid,
      'title'            => $c['title'],
      'bundle'           => $c['bundle'],
      'is_canonical'     => (bool) $c['is_canonical'],
      'task_type'        => $c['task_type'],
      'status'           => $c['status'],
      'similarity_score' => round((float) $c['similarity_score'], 3),
      'url'              => url('node/' . $nid, ['absolute' => TRUE]),
      'lifecycle'        => isset($lifecycle_map[$nid]) ? $lifecycle_map[$nid] : null,
    ];
  }

  return [
    'cluster_id'         => (int) $cluster['cid'],
    'topic_label'        => $cluster['topic_label'],
    'rot_type'           => isset($cluster['rot_type']) ? $cluster['rot_type'] : '',
    'canonical_nid'      => (int) $cluster['canonical_nid'],
    'source_count'       => (int) $cluster['source_count'],
    'similarity_score'   => round((float) $cluster['similarity_score'], 3),
    'age_score'          => round((float) $cluster['age_score'], 3),
    'composite_score'    => round((float) $cluster['composite_score'], 3),
    'recommended_action' => $cluster['recommended_action'],
    'review_state'       => $cluster['review_state'],
    'llm_summary'        => $cluster['llm_summary'] ?: '',
    'candidates'         => $candidate_list,
  ];
}

/**
 * Execute: list runs.
 */
function ai_content_audit_execute_list_runs(array $args) {
  $limit = min(20, max(1, (int) (!empty($args['limit']) ? $args['limit'] : 10)));
  $query = db_select('ai_content_audit_run', 'r');
  $query->fields('r');
  $query->orderBy('started', 'DESC');
  $query->range(0, $limit);
  $runs = $query->execute()->fetchAllAssoc('rid', PDO::FETCH_ASSOC);
  $output = [];
  foreach ($runs as $run) {
    $output[] = [
      'run_id' => $run['run_id'],
      'profile' => $run['profile'],
      'status' => $run['status'],
      'node_count' => (int) $run['node_count'],
      'cluster_count' => (int) $run['cluster_count'],
      'started' => date('Y-m-d H:i:s', $run['started']),
      'finished' => $run['finished'] ? date('Y-m-d H:i:s', $run['finished']) : '',
    ];
  }
  return ['runs' => $output];
}

/**
 * Execute: trigger scan.
 */
function ai_content_audit_execute_trigger_scan(array $args) {
  module_load_include('inc', 'ai_content_audit', 'includes/ai_content_audit.scanner');

  $batch_size = min(200, max(10, (int) (!empty($args['batch_size']) ? $args['batch_size'] : 50)));
  $profile = !empty($args['profile']) ? $args['profile'] : 'default';
  $run_id = ai_content_audit_run_id();

  $result = ai_content_audit_run_scan($run_id, $profile, $batch_size);

  return [
    'run_id' => $run_id,
    'node_count' => $result['node_count'],
    'cluster_count' => $result['cluster_count'],
    'message' => "Scan complete. Use audit_list_clusters to view results.",
  ];
}

/**
 * Execute: propose action for a candidate.
 */
function ai_content_audit_execute_propose_action(array $args) {
  $acid = (int) (!empty($args['acid']) ? $args['acid'] : 0);
  $action = !empty($args['action']) ? $args['action'] : '';
  $reason = !empty($args['reason']) ? $args['reason'] : '';

  $allowed = ['prune', 'flag_for_review', 'merge', 'review', 'edit_queued', 'ignored', 'dismiss'];
  if (!$acid || !in_array($action, $allowed, TRUE)) {
    return ['error' => 'acid and valid action (prune, flag_for_review, dismiss) are required.'];
  }

  $candidate = db_query('SELECT * FROM {ai_content_audit_candidate} WHERE acid = :acid', [':acid' => $acid])->fetchAssoc();
  if (!$candidate) {
    return ['error' => "Candidate $acid not found."];
  }
  if ($candidate['is_canonical']) {
    return ['error' => "Cannot propose action on canonical node. Target a duplicate_candidate."];
  }

  db_update('ai_content_audit_candidate')->fields([
    'task_type' => $action,
    'proposed_action_detail' => substr($reason, 0, 1024),
    'status' => 'pending',
    'updated' => REQUEST_TIME,
  ])->condition('acid', $acid)->execute();

  return [
    'acid' => $acid,
    'action' => $action,
    'status' => 'proposed',
    'message' => "Action '$action' proposed. Requires admin approval at admin/config/ai/agents/content-audit/queue before execution.",
  ];
}

/**
 * Execute: classify ROT for a cluster.
 */
function ai_content_audit_execute_classify_rot(array $args) {
  $cid = (int) (!empty($args['cluster_id']) ? $args['cluster_id'] : 0);
  $rot_type = !empty($args['rot_type']) ? $args['rot_type'] : '';
  $reason = !empty($args['reason']) ? $args['reason'] : '';

  $allowed = ['redundant', 'outdated', 'trivial', 'redundant_outdated', 'review'];
  if (!$cid || !in_array($rot_type, $allowed, TRUE)) {
    return ['error' => 'cluster_id and valid rot_type required.'];
  }

  $fields = ['rot_type' => $rot_type, 'updated' => REQUEST_TIME];
  if ($reason) {
    $existing = db_query('SELECT evidence_summary FROM {ai_content_audit_cluster} WHERE cid = :cid', [':cid' => $cid])->fetchField();
    $fields['evidence_summary'] = substr('ROT: ' . $reason . ($existing ? "\n\n" . $existing : ''), 0, 2048);
  }

  db_update('ai_content_audit_cluster')->fields($fields)->condition('cid', $cid)->execute();

  return ['cluster_id' => $cid, 'rot_type' => $rot_type, 'status' => 'classified'];
}

/**
 * Execute: get node content for reading before proposing edits.
 */
function ai_content_audit_execute_get_node_content(array $args) {
  $nid = (int) (!empty($args['nid']) ? $args['nid'] : 0);
  if (!$nid) {
    return ['error' => 'nid required.'];
  }

  $node = node_load($nid);
  if (!$node) {
    return ['error' => "Node $nid not found."];
  }

  $fields = ai_content_audit_collect_editable_text_fields($node);
  $paragraph_targets = ai_content_audit_collect_paragraph_text_targets($node);
  $primary_edit_target = ai_content_audit_pick_primary_edit_target($node, $fields, $paragraph_targets);
  $primary_edit_field = !empty($primary_edit_target['field_name']) ? $primary_edit_target['field_name'] : '';
  $rendered_content = ai_content_audit_extract_rendered_content($node);

  // Pull ai_content_lifecycle analysis if available.
  $lifecycle = [];
  if (module_exists('ai_content_lifecycle') && db_table_exists('content_lifecycle')) {
    $lc_row = db_query(
      'SELECT review_status, ai_results, changed FROM {content_lifecycle} WHERE entity_type = :type AND entity_id = :nid ORDER BY changed DESC LIMIT 1',
      [':type' => 'node', ':nid' => $nid]
    )->fetchAssoc();
    if ($lc_row) {
      $lifecycle = [
        'review_status' => $lc_row['review_status'],
        'ai_analysis'   => $lc_row['ai_results'],
        'analyzed_date' => $lc_row['changed'] ? date('Y-m-d', $lc_row['changed']) : '',
      ];
    }
  }

  return [
    'nid'       => $nid,
    'vid'       => $node->vid,
    'title'     => $node->title,
    'bundle'    => $node->type,
    'status'    => (int) $node->status,
    'rendered_content' => $rendered_content,
    'primary_edit_target' => $primary_edit_target,
    'primary_edit_field' => $primary_edit_field,
    'fields'    => $fields,
    'paragraph_targets' => $paragraph_targets,
    'lifecycle' => $lifecycle,
    'edit_url'  => url('node/' . $nid . '/edit', ['absolute' => TRUE]),
  ];
}

/**
 * Execute: propose edit — save proposed content as a draft node revision.
 */
function ai_content_audit_execute_propose_edit(array $args) {
  $acid  = (int) (!empty($args['acid']) ? $args['acid'] : 0);
  $nid   = (int) (!empty($args['nid']) ? $args['nid'] : 0);
  $reason = !empty($args['reason']) ? $args['reason'] : '';

  if (!$nid) {
    return ['error' => 'nid required.'];
  }

  $node = node_load($nid);
  if (!$node) {
    return ['error' => "Node $nid not found."];
  }

  $current_vid = $node->vid;
  $target_context = !empty($args['target_context']) && is_array($args['target_context']) ? $args['target_context'] : [];
  if (empty($target_context)) {
    if (!empty($args['target_field'])) {
      $target_context = [
        'target_type' => 'node_field',
        'field_name' => $args['target_field'],
      ];
    }
    else {
      $target_context = ai_content_audit_pick_primary_edit_target($node);
    }
  }
  $target_field = !empty($target_context['field_name']) ? $target_context['field_name'] : '';
  $current_title = $node->title;
  $current_body = '';
  $current_summary = '';
  if ($target_field && !empty($target_context)) {
    $current_body = !empty($target_context['value']) ? $target_context['value'] : '';
    $current_summary = isset($target_context['summary']) ? $target_context['summary'] : '';
  }

  // Decide whether the proposal is substantive BEFORE we mutate the node and
  // BEFORE we write a new paragraph revision. Normalized comparison strips
  // tags + collapses whitespace so cosmetic-only "edits" do not generate a
  // dangling paragraph revision.
  $proposed_title_raw = !empty($args['proposed_title']) ? $args['proposed_title'] : $current_title;
  $proposed_body_raw = !empty($args['proposed_body']) ? $args['proposed_body'] : $current_body;
  $proposed_summary_raw = isset($args['proposed_summary']) ? $args['proposed_summary'] : $current_summary;

  if (
    ai_content_audit_normalize_text_for_compare($proposed_title_raw) === ai_content_audit_normalize_text_for_compare($current_title) &&
    ai_content_audit_normalize_text_for_compare($proposed_body_raw) === ai_content_audit_normalize_text_for_compare($current_body) &&
    ai_content_audit_normalize_text_for_compare($proposed_summary_raw) === ai_content_audit_normalize_text_for_compare($current_summary)
  ) {
    return ['error' => 'No meaningful content change was proposed.'];
  }

  // Apply proposed field changes.
  if (!empty($args['proposed_title'])) {
    $node->title = substr($args['proposed_title'], 0, 255);
  }

  $paragraph_revision_id = 0;
  $paragraph_previous_revision_id = !empty($target_context['revision_id']) ? (int) $target_context['revision_id'] : 0;
  $paragraph_item_id = !empty($target_context['item_id']) ? (int) $target_context['item_id'] : 0;
  if (!empty($args['proposed_body'])) {
    if (!$target_field) {
      return ['error' => 'No editable text field available for proposed content.'];
    }
    $proposed_summary = isset($args['proposed_summary']) ? $args['proposed_summary'] : NULL;
    if (!empty($target_context['target_type']) && $target_context['target_type'] === 'paragraph_field') {
      if (!ai_content_audit_set_paragraph_target_value($node, $target_context, $args['proposed_body'], $proposed_summary)) {
        return ['error' => "Could not write proposed content to paragraph field $target_field."];
      }
      // Note: cannot capture the new paragraph revision_id yet. The paragraph
      // entity will not receive its new revision_id until paragraphs_field_presave
      // fires during node_save() below — reading the host field reference here
      // returns the old revision_id. We re-read after node_save().
    }
    elseif (!ai_content_audit_set_text_field_value($node, $target_field, $args['proposed_body'], $proposed_summary)) {
      return ['error' => "Could not write proposed content to field $target_field."];
    }
  }

  // Save as a draft revision. When draft_workflow is enabled, is_active_revision
  // = FALSE keeps the previously published revision as the current one so the
  // live site keeps rendering the original until an admin explicitly activates
  // the draft.
  $node->revision = TRUE;
  $node->is_new_revision = TRUE;
  if (module_exists('draft_workflow')) {
    $node->is_active_revision = FALSE;
    // Preserve the existing published status — the draft itself is not what
    // controls visibility; the active revision does.
    $original_status = isset($node->original->status) ? $node->original->status : $node->status;
    $node->status = $original_status;
  }
  else {
    // Without draft_workflow we can't keep the old revision active, so at
    // least mark this revision unpublished.
    $node->status = 0;
  }
  $node->log = 'AI proposed edit: ' . substr($reason, 0, 255);

  try {
    node_save($node);
  }
  catch (\Exception $e) {
    return ['error' => 'Could not save proposed revision: ' . $e->getMessage()];
  }

  $proposed_vid = $node->vid;

  // Now that node_save() has fired paragraphs_field_presave, the paragraph
  // entity has its new revision_id. Capture it for before_state so reject can
  // unwind the orphan paragraph revision.
  if ($paragraph_item_id && module_exists('paragraphs')) {
    $paragraph_revision_id = (int) db_query(
      'SELECT revision_id FROM {paragraphs_item} WHERE item_id = :id',
      [':id' => $paragraph_item_id]
    )->fetchField();
  }

  $review_url = url('node/' . $nid . '/revisions/' . $proposed_vid . '/edit_revision', ['absolute' => TRUE]);
  $revisions_url = url('node/' . $nid . '/revisions', ['absolute' => TRUE]);
  $activate_url = url('node/' . $nid . '/revisions/' . $proposed_vid . '/activate', ['absolute' => TRUE]);
  $diff_url = module_exists('diff')
    ? url('node/' . $nid . '/revisions/view/' . $current_vid . '/' . $proposed_vid, ['absolute' => TRUE])
    : '';

  // Update the candidate record if an acid was provided.
  if ($acid) {
    $candidate = db_query('SELECT task_type, status FROM {ai_content_audit_candidate} WHERE acid = :acid', [':acid' => $acid])->fetchAssoc();
    db_update('ai_content_audit_candidate')->fields([
      'task_type'             => 'edit_proposed',
      'before_state'          => json_encode([
        'current_vid' => $current_vid,
        'proposed_vid' => $proposed_vid,
        'target_field' => $target_field,
        'target_context' => $target_context,
        'paragraph_item_id' => $paragraph_item_id,
        'paragraph_previous_revision_id' => $paragraph_previous_revision_id,
        'paragraph_proposed_revision_id' => $paragraph_revision_id,
        'original_task_type' => !empty($candidate['task_type']) ? $candidate['task_type'] : '',
        'original_status' => !empty($candidate['status']) ? $candidate['status'] : 'pending',
      ]),
      'proposed_action_detail' => substr($reason, 0, 1024),
      'status'                => 'pending',
      'updated'               => REQUEST_TIME,
    ])->condition('acid', $acid)->execute();
  }

  return [
    'nid'          => $nid,
    'current_vid'  => $current_vid,
    'proposed_vid' => $proposed_vid,
    'review_url'   => $review_url,
    'revisions_url' => $revisions_url,
    'activate_url' => $activate_url,
    'diff_url'     => $diff_url,
    'message'      => "Proposed forward revision saved. Review it in Draft Workflow at: $review_url",
  ];
}

// Field-level helpers (text-field readers/writers, paragraph target picking,
// normalize_text_for_compare) live in ai_content_audit.fields.inc.

/**
 * Extract rendered main content from a node using the shared AI extractor.
 */
function ai_content_audit_extract_rendered_content($node) {
  require_once backdrop_get_path('module', 'ai') . '/includes/AIContentExtractor.php';
  return AIContentExtractor::extractEntityText('node', $node, [
    'view_mode' => 'full',
    'include_label' => FALSE,
  ]);
}

/**
 * Execute: dismiss cluster.
 */
function ai_content_audit_execute_dismiss_cluster(array $args) {
  $cid = (int) (!empty($args['cluster_id']) ? $args['cluster_id'] : 0);
  if (!$cid) {
    return ['error' => 'cluster_id required.'];
  }
  $reason = !empty($args['reason']) ? $args['reason'] : '';

  db_update('ai_content_audit_cluster')->fields([
    'review_state' => 'dismissed',
    'evidence_summary' => substr($reason, 0, 2048),
    'updated' => REQUEST_TIME,
  ])->condition('cid', $cid)->execute();

  return ['cluster_id' => $cid, 'status' => 'dismissed', 'message' => "Cluster $cid dismissed."];
}
