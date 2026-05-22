<?php

/**
 * @file
 * Admin forms for AI Content Audit.
 *
 * All forms live under Admin → Configuration → AI → AI Content Audit and are
 * gated by the 'administer ai content audit' permission (see hook_menu in
 * the module file).
 *
 * Forms:
 *   - ai_content_audit_dashboard_form    — Overview tab. Cluster counts,
 *                                           "Scan Now" button, queue summary.
 *   - ai_content_audit_runs_form         — Runs tab. List recent scan runs
 *                                           with status, counts, timestamps.
 *   - ai_content_audit_clusters_form     — Clusters tab. Ranked cluster list
 *                                           with bulk approve / reject / delete.
 *   - ai_content_audit_cluster_detail_form — One cluster's candidates, scores,
 *                                             canonical pick, LLM summary,
 *                                             per-candidate actions including
 *                                             "Propose AI edit" (→ llm.inc).
 *   - ai_content_audit_queue_form        — Action queue. Approve / reject
 *                                           pending candidates incl. edit_proposed
 *                                           draft revisions with diff links.
 *   - ai_content_audit_settings_form     — Settings: similarity threshold,
 *                                           cron, LLM model, auto-draft, GA4
 *                                           traffic CSV upload, bundle filter.
 *
 * Also defines the proposed-edit diff page callback
 * (ai_content_audit_proposed_edit_diff_page) for the per-candidate text diff.
 */

/**
 * Dashboard form.
 */
function ai_content_audit_dashboard_form($form, &$form_state) {
  $counts = ai_content_audit_cluster_counts();
  $latest_run = db_query('SELECT * FROM {ai_content_audit_run} ORDER BY started DESC LIMIT 1')->fetchAssoc();

  $form['intro'] = [
    '#markup' => '<p>' . t('Finds repetitive and overlapping content clusters. Produces ranked reports for human review before any changes are applied.') . '</p>',
  ];

  $traffic_uploaded = config_get('ai_content_audit.settings', 'traffic_uploaded');
  $traffic_count    = config_get('ai_content_audit.settings', 'traffic_path_count');
  $traffic_notice   = $traffic_uploaded
    ? t('Traffic data: <strong>@count path(s)</strong> uploaded <strong>@date</strong> — scores reflect that snapshot.', [
        '@count' => $traffic_count,
        '@date'  => format_date($traffic_uploaded, 'short'),
      ])
    : t('Traffic data: <em>none uploaded</em> — traffic scores will be 0. Upload a GA4 CSV on the <a href="@url">Settings</a> tab.', [
        '@url' => url('admin/config/ai/content-audit/settings'),
      ]);
  $form['snapshot'] = [
    '#markup' => '<div class="messages status"><p>' . $traffic_notice . '</p></div>',
  ];

  if ($latest_run) {
    $form['latest'] = [
      '#markup' => '<p>' . t('Last scan: <strong>@date</strong> — @nodes node(s), @clusters cluster(s), status: @status. Cluster scores reflect site content <em>as of that date</em>.', [
        '@date'     => format_date($latest_run['started'], 'medium'),
        '@nodes'    => $latest_run['node_count'],
        '@clusters' => $latest_run['cluster_count'],
        '@status'   => $latest_run['status'],
      ]) . '</p>',
    ];
  }

  $form['cluster_counts'] = [
    '#theme' => 'table',
    '#header' => [t('Review State'), t('Clusters')],
    '#rows' => [
      [t('Pending'), isset($counts['pending']) ? $counts['pending'] : 0],
      [t('Dismissed'), isset($counts['dismissed']) ? $counts['dismissed'] : 0],
      [t('Actioned'), isset($counts['actioned']) ? $counts['actioned'] : 0],
    ],
    '#empty' => t('No clusters yet. Run a scan.'),
  ];

  // Show in-progress scan status if a run is collecting.
  $active_run = db_query(
    "SELECT * FROM {ai_content_audit_run} WHERE status = 'collecting' ORDER BY started DESC LIMIT 1"
  )->fetchAssoc();
  if ($active_run) {
    $pct = $active_run['total_nodes'] > 0
      ? round(($active_run['processed_nodes'] / $active_run['total_nodes']) * 100)
      : 0;
    $form['scan_progress'] = [
      '#markup' => '<div class="messages status"><p>'
        . t('Scan in progress: <strong>@done of @total</strong> nodes processed (@pct%).', [
            '@done'  => $active_run['processed_nodes'],
            '@total' => $active_run['total_nodes'],
            '@pct'   => $pct,
          ])
        . '</p></div>',
    ];
  }

  $config = config('ai_content_audit.settings');
  $form['actions'] = ['#type' => 'actions'];
  $form['actions']['scan_now'] = [
    '#type'  => 'submit',
    '#value' => $active_run ? t('Process next batch now') : t('Scan now'),
    '#op'    => 'scan',
  ];
  if (!empty($counts['pending'])) {
    $form['actions']['apply_approved'] = [
      '#type' => 'submit',
      '#value' => t('Apply approved actions'),
      '#op' => 'apply',
    ];
  }

  $top_clusters = db_query(
    'SELECT c.*, r.started FROM {ai_content_audit_cluster} c
     LEFT JOIN {ai_content_audit_run} r ON r.run_id = c.run_id
     WHERE c.review_state = :state
     ORDER BY c.composite_score DESC LIMIT 10',
    [':state' => 'pending']
  )->fetchAllAssoc('cid', PDO::FETCH_ASSOC);

  $live_counts = ai_content_audit_live_cluster_counts(array_keys($top_clusters));
  $rows = [];
  foreach ($top_clusters as $cluster) {
    $live = isset($live_counts[$cluster['cid']]) ? (int) $live_counts[$cluster['cid']] : 0;
    $rows[] = [
      l(check_plain($cluster['topic_label']), 'admin/config/ai/content-audit/clusters/' . $cluster['cid']),
      $live,
      round($cluster['composite_score'], 2),
      check_plain($cluster['recommended_action']),
      $cluster['started'] ? format_date($cluster['started'], 'short') : '',
    ];
  }

  $form['top_clusters'] = [
    '#theme' => 'table',
    '#caption' => t('Top pending clusters by score'),
    '#header' => [t('Topic'), t('Pages'), '<abbr title="' . t('Composite priority score 0–1: Similarity 50% + Age 30% + Traffic 20%. Higher = more urgent.') . '">' . t('Score') . '</abbr>', t('Action'), t('Run date')],
    '#rows' => $rows,
    '#empty' => t('No pending clusters.'),
  ];

  return $form;
}

/**
 * Build Draft Workflow review links for a proposed revision.
 */
function ai_content_audit_proposed_revision_links($nid, $before_state, $acid = 0) {
  $links = [];
  $vids = $before_state ? json_decode($before_state, TRUE) : [];
  $proposed_vid = !empty($vids['proposed_vid']) ? (int) $vids['proposed_vid'] : 0;
  $current_vid = !empty($vids['current_vid']) ? (int) $vids['current_vid'] : 0;

  if (!$nid || !$proposed_vid) {
    return $links;
  }

  // Audit-local text diff comes first so it's the obvious default. The other
  // three links leave the audit UI for the node's own revision pages.
  if ($acid) {
    $links[] = l(t('[text diff]'), 'admin/config/ai/content-audit/proposed-edit/' . $acid . '/diff', ['attributes' => ['target' => '_blank']]);
  }
  elseif ($current_vid && module_exists('diff')) {
    $links[] = l(t('[text diff]'), 'node/' . $nid . '/revisions/view/' . $current_vid . '/' . $proposed_vid, ['attributes' => ['target' => '_blank']]);
  }

  if (module_exists('draft_workflow')) {
    $links[] = l(t('[review draft (node)]'), 'node/' . $nid . '/revisions/' . $proposed_vid . '/edit_revision', ['attributes' => ['target' => '_blank']]);
    $links[] = l(t('[revisions tab (node)]'), 'node/' . $nid . '/revisions', ['attributes' => ['target' => '_blank']]);
    $links[] = l(t('[make current (node)]'), 'node/' . $nid . '/revisions/' . $proposed_vid . '/activate', ['attributes' => ['target' => '_blank']]);
  }

  return $links;
}

/**
 * Dashboard submit.
 */
function ai_content_audit_dashboard_form_submit($form, &$form_state) {
  $op = !empty($form_state['triggering_element']['#op']) ? $form_state['triggering_element']['#op'] : '';

  if ($op === 'scan') {
    module_load_include('inc', 'ai_content_audit', 'includes/ai_content_audit.scanner');
    $config = config('ai_content_audit.settings');
    $batch_size = (int) $config->get('cron_batch_size') ?: 50;

    $active = ai_content_audit_active_run();
    if (!$active) {
      $profile = $config->get('default_profile') ?: 'default';
      ai_content_audit_start_run($profile);
      $active = ai_content_audit_active_run();
    }
    $result = ai_content_audit_process_batch($active['run_id'], $batch_size);
    if ($result['finalized']) {
      backdrop_set_message(t('Scan complete: @nodes node(s) processed, @clusters cluster(s) found.', [
        '@nodes'    => $result['total'],
        '@clusters' => $result['cluster_count'] ?? '?',
      ]));
    }
    else {
      backdrop_set_message(t('Batch processed: @done of @total node(s). Click Scan now to continue.', [
        '@done'  => $result['processed'],
        '@total' => $result['total'],
      ]));
    }
  }
  elseif ($op === 'apply') {
    $applied = ai_content_audit_apply_approved(10);
    backdrop_set_message(t('Applied @count approved action(s).', ['@count' => $applied]));
  }

  $form_state['redirect'] = 'admin/config/ai/content-audit';
}

/**
 * Runs list form.
 */
function ai_content_audit_runs_form($form, &$form_state) {
  $runs = db_query('SELECT * FROM {ai_content_audit_run} ORDER BY started DESC LIMIT 50')->fetchAllAssoc('rid', PDO::FETCH_ASSOC);

  $rows = [];
  foreach ($runs as $run) {
    $duration = $run['finished'] ? ($run['finished'] - $run['started']) . 's' : t('—');
    $progress = ($run['status'] === 'collecting' && $run['total_nodes'] > 0)
      ? $run['processed_nodes'] . '/' . $run['total_nodes']
      : (int) $run['node_count'];
    $rows[] = [
      check_plain($run['run_id']),
      check_plain($run['profile']),
      check_plain($run['status']),
      $progress,
      (int) $run['cluster_count'],
      format_date($run['started'], 'short'),
      $duration,
    ];
  }

  $form['runs'] = [
    '#theme' => 'table',
    '#header' => [t('Run ID'), t('Profile'), t('Status'), t('Nodes'), t('Clusters'), t('Started'), t('Duration')],
    '#rows' => $rows,
    '#empty' => t('No audit runs yet.'),
  ];

  return $form;
}

/**
 * Clusters list form.
 */
function ai_content_audit_clusters_form($form, &$form_state) {
  $review_state = !empty($_GET['state']) ? $_GET['state'] : 'pending';

  $form['filter'] = [
    '#type' => 'select',
    '#title' => t('Review state'),
    '#options' => [
      'pending' => t('Pending'),
      'dismissed' => t('Dismissed'),
      'actioned' => t('Actioned'),
    ],
    '#default_value' => $review_state,
    '#attributes' => ['onchange' => 'this.form.submit()'],
  ];
  $form['filter_submit'] = ['#type' => 'submit', '#value' => t('Filter'), '#attributes' => ['class' => ['element-invisible']]];

  $header = [
    ['data' => t('Topic'),      'field' => 'c.topic_label'],
    ['data' => t('Pages'),      'field' => 'c.source_count'],
    ['data' => t('ROT'),        'field' => 'c.rot_type'],
    ['data' => t('Similarity'), 'field' => 'c.similarity_score'],
    ['data' => t('Age'),        'field' => 'c.age_score'],
    ['data' => t('Traffic'),    'field' => 'c.traffic_score'],
    ['data' => t('Score'),      'field' => 'c.composite_score', 'sort' => 'desc'],
    ['data' => t('Action'),     'field' => 'c.recommended_action'],
    ['data' => t('Scan date'),  'field' => 'r.started'],
  ];

  $query = db_select('ai_content_audit_cluster', 'c')->extend('TableSort');
  $query->leftJoin('ai_content_audit_run', 'r', 'r.run_id = c.run_id');
  $query->fields('c');
  $query->addField('r', 'started', 'run_started');
  $query->condition('c.review_state', $review_state);
  $query->orderByHeader($header);
  $query->range(0, 100);
  $clusters = $query->execute()->fetchAllAssoc('cid', PDO::FETCH_ASSOC);

  $rot_labels = [
    'redundant'          => t('Redundant'),
    'outdated'           => t('Outdated'),
    'trivial'            => t('Trivial'),
    'redundant_outdated' => t('Redundant + Outdated'),
    'review'             => t('Review'),
    ''                   => t('—'),
  ];

  $live_counts = ai_content_audit_live_cluster_counts(array_keys($clusters));
  $options = [];
  foreach ($clusters as $cluster) {
    $rot_display = isset($rot_labels[$cluster['rot_type']]) ? $rot_labels[$cluster['rot_type']] : check_plain($cluster['rot_type']);
    $live = isset($live_counts[$cluster['cid']]) ? (int) $live_counts[$cluster['cid']] : 0;
    $options[$cluster['cid']] = [
      l(check_plain($cluster['topic_label']), 'admin/config/ai/content-audit/clusters/' . $cluster['cid']),
      $live,
      $rot_display,
      round($cluster['similarity_score'], 3),
      round($cluster['age_score'], 3),
      round($cluster['traffic_score'], 3),
      round($cluster['composite_score'], 3),
      check_plain($cluster['recommended_action']),
      $cluster['run_started'] ? format_date($cluster['run_started'], 'short') : '—',
    ];
  }

  $form['score_legend'] = [
    '#markup' => '<details style="margin-bottom:1em"><summary>' . t('Score &amp; ROT column guide') . '</summary><dl>'
      . '<dt>' . t('ROT') . '</dt><dd>' . t('<strong>Redundant</strong> = near-identical content, both recent — consolidate or prune. <strong>Outdated</strong> = same topic but pages span years — the older page may need updating or archiving. <strong>Trivial</strong> = loosely related, low value — low priority. <strong>Redundant+Outdated</strong> = duplicate AND stale — strong prune candidate.') . '</dd>'
      . '<dt>' . t('Similarity (0–1)') . '</dt><dd>' . t('How alike the pages are. 1.0 = near-identical; 0.5 = loosely related.') . '</dd>'
      . '<dt>' . t('Age (0–1)') . '</dt><dd>' . t('Fraction of pages last updated more than 2 years ago. 1.0 = all stale.') . '</dd>'
      . '<dt>' . t('Traffic (0–1)') . '</dt><dd>' . t('Normalized GA4 pageviews. 1.0 = highest-traffic cluster.') . '</dd>'
      . '<dt>' . t('Score (0–1)') . '</dt><dd>' . t('Composite: Similarity 50% + Age 30% + Traffic 20%. Higher = more urgent.') . '</dd>'
      . '<dt>' . t('Action') . '</dt><dd>' . t('<strong>prune</strong> = unpublish duplicate; <strong>merge</strong> = consolidate; <strong>refresh</strong> = update stale content; <strong>review</strong> = human judgement needed.') . '</dd>'
      . '</dl></details>',
  ];

  $form['clusters'] = [
    '#type' => 'tableselect',
    '#header' => $header,
    '#options' => $options,
    '#empty' => t('No clusters in this state.'),
  ];

  $form['actions'] = ['#type' => 'actions'];
  $form['actions']['delete'] = [
    '#type' => 'submit',
    '#value' => t('Delete selected'),
    '#op' => 'delete',
  ];

  return $form;
}

/**
 * Clusters list submit.
 */
function ai_content_audit_clusters_form_submit($form, &$form_state) {
  $op = !empty($form_state['triggering_element']['#op']) ? $form_state['triggering_element']['#op'] : '';

  if ($op === 'delete') {
    $selected = [];
    if (!empty($form_state['values']['clusters']) && is_array($form_state['values']['clusters'])) {
      foreach ($form_state['values']['clusters'] as $cid => $checked) {
        if ($checked) {
          $selected[] = (int) $cid;
        }
      }
    }
    if (empty($selected)) {
      backdrop_set_message(t('No clusters selected.'), 'warning');
    }
    else {
      ai_content_audit_delete_clusters($selected);
      backdrop_set_message(t('Deleted @count cluster(s) and their candidates.', ['@count' => count($selected)]));
    }
  }

  $form_state['redirect'] = 'admin/config/ai/content-audit/clusters';
}

/**
 * Cluster detail form.
 */
function ai_content_audit_cluster_detail_form($form, &$form_state, $cid) {
  $cluster = ai_content_audit_load_cluster((int) $cid);
  if (!$cluster) {
    backdrop_set_message(t('Cluster not found.'), 'error');
    backdrop_goto('admin/config/ai/content-audit/clusters');
  }

  $form['#cluster'] = $cluster;

  $rot_labels = [
    'redundant'          => t('Redundant'),
    'outdated'           => t('Outdated'),
    'trivial'            => t('Trivial'),
    'redundant_outdated' => t('Redundant + Outdated'),
    'review'             => t('Review'),
  ];
  $rot_descriptions = [
    'redundant'          => t('Near-identical content, both relatively recent. Consider pruning or merging.'),
    'outdated'           => t('Same topic but pages span years. The older page(s) may need updating or archiving.'),
    'trivial'            => t('Loosely related, low value overlap. Low priority.'),
    'redundant_outdated' => t('Duplicate content AND stale. Strong candidate for pruning.'),
    'review'             => t('Needs human judgement to classify.'),
  ];
  $rot_colors = [
    'redundant'          => '#c0392b',
    'outdated'           => '#e67e22',
    'trivial'            => '#7f8c8d',
    'redundant_outdated' => '#7b0000',
    'review'             => '#2980b9',
  ];
  $rot_type = !empty($cluster['rot_type']) ? $cluster['rot_type'] : 'review';
  $rot_label = isset($rot_labels[$rot_type]) ? $rot_labels[$rot_type] : check_plain($rot_type);
  $rot_description = isset($rot_descriptions[$rot_type]) ? $rot_descriptions[$rot_type] : '';
  $rot_color = isset($rot_colors[$rot_type]) ? $rot_colors[$rot_type] : '#2980b9';
  $rot_badge = '<span style="display:inline-block;padding:3px 10px;border-radius:3px;background:' . $rot_color . ';color:#fff;font-weight:bold;font-size:0.9em;letter-spacing:0.03em">'
    . check_plain($rot_label) . '</span>';

  $form['meta'] = [
    '#markup' => '<div style="margin-bottom:1em">'
      . '<div style="margin-bottom:0.5em">' . $rot_badge . ' &nbsp; <em>' . $rot_description . '</em></div>'
      . '</div>'
      . '<dl>'
      . '<dt>' . t('Topic') . '</dt><dd>' . check_plain($cluster['topic_label']) . '</dd>'
      . '<dt>' . t('Recommended action') . '</dt><dd>' . check_plain($cluster['recommended_action'])
        . ' <small>(' . t('prune = near-duplicate; merge = overlapping; refresh = stale; review = needs judgement') . ')</small></dd>'
      . '<dt>' . t('Similarity score') . '</dt><dd>' . round($cluster['similarity_score'], 3)
        . ' <small>(' . t('0–1; how alike the pages are') . ')</small></dd>'
      . '<dt>' . t('Age score') . '</dt><dd>' . round($cluster['age_score'], 3)
        . ' <small>(' . t('fraction of pages not updated in 2+ years') . ')</small></dd>'
      . '<dt>' . t('Traffic score') . '</dt><dd>' . round($cluster['traffic_score'], 3)
        . ' <small>(' . t('normalized GA4 pageviews; requires CSV upload') . ')</small></dd>'
      . '<dt>' . t('Composite score') . '</dt><dd>' . round($cluster['composite_score'], 3)
        . ' <small>(' . t('Similarity 50% + Age 30% + Traffic 20%') . ')</small></dd>'
      . '</dl>',
  ];

  if (!empty($cluster['llm_summary'])) {
    $form['summary'] = [
      '#markup' => '<blockquote>' . check_plain($cluster['llm_summary']) . '</blockquote>',
    ];
  }

  $candidates = ai_content_audit_load_candidates($cid);

  // Collect all nids in this cluster (candidates + canonical).
  $all_nids = array_values(array_unique(array_merge(
    array_column($candidates, 'nid'),
    [(int) $cluster['canonical_nid']]
  )));

  // Load lifecycle id and status for all nids in one query, keyed by nid.
  $lifecycle_data = [];
  if (!empty($all_nids) && module_exists('ai_content_lifecycle') && db_table_exists('content_lifecycle')) {
    $result = db_query(
      'SELECT entity_id, id, review_status FROM {content_lifecycle} WHERE entity_type = :type AND entity_id IN (:nids)',
      [':type' => 'node', ':nids' => $all_nids]
    );
    while ($row = $result->fetchAssoc()) {
      $lifecycle_data[(int) $row['entity_id']] = $row;
    }
  }

  // Load live node data (title + changed) for all nids in the cluster.
  $two_years = 60 * 60 * 24 * 365 * 2;
  $node_age = [];
  $live_titles = [];
  if (!empty($all_nids)) {
    $node_rows = db_query(
      'SELECT nid, title, changed FROM {node} WHERE nid IN (:nids)',
      [':nids' => $all_nids]
    )->fetchAllAssoc('nid', PDO::FETCH_ASSOC);
    foreach ($node_rows as $nid => $row) {
      $live_titles[(int) $nid] = $row['title'];
      $node_age[(int) $nid] = (REQUEST_TIME - (int) $row['changed']) > $two_years ? 1.0 : 0.0;
    }
  }

  // Per-node traffic: look up pageviews from uploaded GA4 data.
  $node_traffic = array_fill_keys($all_nids, 0);
  if (!empty($all_nids) && db_table_exists('ai_content_audit_traffic') && db_query('SELECT COUNT(*) FROM {ai_content_audit_traffic}')->fetchField()) {
    $sources = array_map(function($nid) { return 'node/' . $nid; }, $all_nids);
    $aliases = db_query(
      'SELECT source, alias FROM {url_alias} WHERE source IN (:sources)',
      [':sources' => $sources]
    )->fetchAllKeyed();
    $paths_by_nid = [];
    foreach ($all_nids as $nid) {
      $paths_by_nid[$nid] = ['/node/' . $nid];
      $src = 'node/' . $nid;
      if (!empty($aliases[$src])) {
        $paths_by_nid[$nid][] = '/' . ltrim($aliases[$src], '/');
      }
    }
    $all_paths = array_values(array_unique(array_merge(...array_values($paths_by_nid))));
    $traffic_rows = db_query(
      'SELECT path, pageviews FROM {ai_content_audit_traffic} WHERE path IN (:paths)',
      [':paths' => $all_paths]
    )->fetchAllKeyed();
    foreach ($all_nids as $nid) {
      $views = 0;
      foreach ($paths_by_nid[$nid] as $path) {
        if (!empty($traffic_rows[$path])) {
          $views = max($views, (int) $traffic_rows[$path]);
        }
      }
      $node_traffic[$nid] = $views;
    }
  }

  $options = [];
  foreach ($candidates as $c) {
    $nid = (int) $c['nid'];
    $display_title = isset($live_titles[$nid]) ? $live_titles[$nid] : $c['title'];
    $node_link = l(check_plain($display_title), 'node/' . $nid, ['attributes' => ['target' => '_blank']]);
    $is_canonical = !empty($c['is_canonical']);
    $nid_label = '<span style="color:#999;font-size:0.85em;margin-left:5px">#' . $nid . '</span>';
    $title_cell = $node_link . $nid_label;

    // Add Draft Workflow review links for edit_proposed candidates.
    if ($c['task_type'] === 'edit_proposed' && !empty($c['before_state'])) {
      $review_links = ai_content_audit_proposed_revision_links($nid, $c['before_state'], (int) $c['acid']);
      if ($review_links) {
        $title_cell .= ' ' . implode(' ', $review_links);
      }
    }

    if (isset($lifecycle_data[$nid])) {
      $lc = $lifecycle_data[$nid];
      $lifecycle = l(check_plain($lc['review_status']), 'admin/content/ai-content-lifecycle/' . $lc['id']);
    }
    else {
      $lifecycle = '—';
    }
    $options[$c['acid']] = [
      $title_cell,
      check_plain($c['bundle']),
      check_plain($c['task_type']),
      check_plain($c['status']),
      round($c['similarity_score'], 3),
      isset($node_age[$nid]) ? round($node_age[$nid], 3) : '—',
      isset($node_traffic[$nid]) ? number_format($node_traffic[$nid]) : '—',
      round($cluster['composite_score'], 3),
      ['data' => $rot_badge],
      check_plain($cluster['recommended_action']),
      ['data' => $lifecycle],
    ];
  }

  $form['candidates'] = [
    '#type' => 'tableselect',
    '#header' => [t('Title'), t('Type'), t('Task'), t('Status'), t('Similarity'), t('Age'), t('Traffic'), t('Score'), t('ROT'), t('Action'), t('Lifecycle')],
    '#options' => $options,
    '#empty' => t('No candidates.'),
  ];

  $form['actions'] = ['#type' => 'actions'];
  $form['actions']['approve'] = [
    '#type' => 'submit',
    '#value' => t('Approve selected'),
    '#op' => 'approve',
  ];
  $form['actions']['edit_queued'] = [
    '#type' => 'submit',
    '#value' => t('Mark for editing'),
    '#op' => 'edit_queued',
  ];
  $form['actions']['propose_ai_edit'] = [
    '#type' => 'submit',
    '#value' => t('Propose AI edit'),
    '#op' => 'propose_ai_edit',
  ];
  $form['actions']['ignore'] = [
    '#type' => 'submit',
    '#value' => t('Ignore selected'),
    '#op' => 'ignored',
  ];
  $form['actions']['reject'] = [
    '#type' => 'submit',
    '#value' => t('Reject selected'),
    '#op' => 'reject',
  ];
  $form['actions']['dismiss_cluster'] = [
    '#type' => 'submit',
    '#value' => t('Dismiss cluster'),
    '#op' => 'dismiss',
  ];
  $form['actions']['delete_cluster'] = [
    '#type' => 'submit',
    '#value' => t('Delete cluster'),
    '#op' => 'delete',
    '#attributes' => ['onclick' => 'return confirm("Permanently delete this cluster and all its candidates?")'],
  ];

  return $form;
}

/**
 * Cluster detail submit.
 */
function ai_content_audit_cluster_detail_form_submit($form, &$form_state) {
  $op = !empty($form_state['triggering_element']['#op']) ? $form_state['triggering_element']['#op'] : '';
  $cid = !empty($form['#cluster']['cid']) ? (int) $form['#cluster']['cid'] : 0;

  if ($op === 'dismiss') {
    db_update('ai_content_audit_cluster')->fields([
      'review_state' => 'dismissed',
      'updated' => REQUEST_TIME,
    ])->condition('cid', $cid)->execute();
    backdrop_set_message(t('Cluster dismissed.'));
    $form_state['redirect'] = 'admin/config/ai/content-audit/clusters';
    return;
  }

  if ($op === 'delete') {
    ai_content_audit_delete_clusters([$cid]);
    backdrop_set_message(t('Cluster deleted.'));
    $form_state['redirect'] = 'admin/config/ai/content-audit/clusters';
    return;
  }

  if ($op === 'propose_ai_edit') {
    module_load_include('inc', 'ai_content_audit', 'includes/ai_content_audit.llm');
    module_load_include('inc', 'ai_content_audit', 'includes/ai_content_audit.tools');
    $cluster = ai_content_audit_load_cluster($cid);
    if ($cluster && ai_content_audit_auto_draft_cluster_edit($cluster)) {
      backdrop_set_message(t('AI draft revision proposed for the canonical page. Review the diff and approve it from the queue or this cluster view.'));
    }
    else {
      backdrop_set_message(t('No AI draft revision was created for this cluster. The canonical page may lack editable body content, already have a pending draft, or the model may have declined to propose changes.'), 'warning');
    }
    $form_state['redirect'] = 'admin/config/ai/content-audit/clusters/' . $cid;
    return;
  }

  $selected = [];
  if (!empty($form_state['values']['candidates']) && is_array($form_state['values']['candidates'])) {
    foreach ($form_state['values']['candidates'] as $acid => $checked) {
      if ($checked) {
        $selected[] = (int) $acid;
      }
    }
  }

  if (empty($selected)) {
    backdrop_set_message(t('No candidates selected.'), 'warning');
    return;
  }

  if ($op === 'approve') {
    ai_content_audit_update_candidate_status($selected, 'approved', $GLOBALS['user']->uid);
    backdrop_set_message(t('Approved @count candidate(s). Apply from the dashboard when ready.', ['@count' => count($selected)]));
  }
  elseif ($op === 'edit_queued') {
    ai_content_audit_update_candidate_status($selected, 'edit_queued');
    $links = [];
    $rows = db_query('SELECT acid, nid, title FROM {ai_content_audit_candidate} WHERE acid IN (:acids)', [':acids' => $selected])->fetchAllAssoc('acid', PDO::FETCH_ASSOC);
    foreach ($rows as $row) {
      $links[] = l(check_plain($row['title']), 'node/' . $row['nid'] . '/edit', ['attributes' => ['target' => '_blank']]);
    }
    backdrop_set_message(t('Marked @count candidate(s) for editing: !links', ['@count' => count($selected), '!links' => implode(', ', $links)]));
  }
  elseif ($op === 'ignored') {
    ai_content_audit_update_candidate_status($selected, 'ignored');
    backdrop_set_message(t('Ignored @count candidate(s).', ['@count' => count($selected)]));
  }
  elseif ($op === 'reject') {
    ai_content_audit_update_candidate_status($selected, 'rejected');
    backdrop_set_message(t('Rejected @count candidate(s).', ['@count' => count($selected)]));
  }

  $form_state['redirect'] = 'admin/config/ai/content-audit/clusters/' . $cid;
}

/**
 * Action queue form — staged actions only.
 */
function ai_content_audit_queue_form($form, &$form_state) {
  $candidates = db_query(
    'SELECT ca.*, c.topic_label FROM {ai_content_audit_candidate} ca
     LEFT JOIN {ai_content_audit_cluster} c ON c.cid = ca.cluster_id
     WHERE (
       (ca.status = :pending_status AND ca.task_type = :pending_task)
       OR ca.status IN (:statuses)
     )
     ORDER BY ca.updated DESC LIMIT 200',
    [
      ':pending_status' => 'pending',
      ':pending_task' => 'edit_proposed',
      ':statuses' => ['approved', 'applied', 'rejected', 'failed', 'edit_queued', 'ignored'],
    ]
  )->fetchAllAssoc('acid', PDO::FETCH_ASSOC);

  $options = [];
  foreach ($candidates as $c) {
    $node_link = l(check_plain($c['title']), 'node/' . $c['nid'], ['attributes' => ['target' => '_blank']]);
    if ($c['status'] === 'edit_queued') {
      $node_link .= ' ' . l(t('[edit]'), 'node/' . $c['nid'] . '/edit', ['attributes' => ['target' => '_blank']]);
    }
    if ($c['task_type'] === 'edit_proposed' && !empty($c['before_state'])) {
      $review_links = ai_content_audit_proposed_revision_links((int) $c['nid'], $c['before_state'], (int) $c['acid']);
      if ($review_links) {
        $node_link .= ' ' . implode(' ', $review_links);
      }
    }
    $status_display = in_array($c['status'], ['approved', 'edit_queued'], TRUE)
      ? '<strong>' . check_plain($c['status']) . '</strong>'
      : check_plain($c['status']);
    $options[$c['acid']] = [
      ['data' => $status_display],
      check_plain($c['task_type']),
      $node_link,
      check_plain($c['topic_label']),
      format_date($c['updated'], 'short'),
    ];
  }

  $form['queue'] = [
    '#type' => 'tableselect',
    '#header' => [t('Status'), t('Task'), t('Node'), t('Cluster topic'), t('Updated')],
    '#options' => $options,
    '#empty' => t('No candidates in queue yet.'),
  ];

  $form['actions'] = ['#type' => 'actions'];
  $form['actions']['apply'] = [
    '#type' => 'submit',
    '#value' => t('Apply approved'),
    '#op' => 'apply',
  ];
  $form['actions']['reject'] = [
    '#type' => 'submit',
    '#value' => t('Reject selected'),
    '#op' => 'reject',
  ];

  return $form;
}

/**
 * Queue form submit.
 */
function ai_content_audit_queue_form_submit($form, &$form_state) {
  $op = !empty($form_state['triggering_element']['#op']) ? $form_state['triggering_element']['#op'] : '';

  $selected = [];
  if (!empty($form_state['values']['queue']) && is_array($form_state['values']['queue'])) {
    foreach ($form_state['values']['queue'] as $acid => $checked) {
      if ($checked) {
        $selected[] = (int) $acid;
      }
    }
  }

  if ($op === 'apply') {
    $applied = ai_content_audit_apply_approved(count($selected) ?: 10);
    backdrop_set_message(t('Applied @count action(s).', ['@count' => $applied]));
  }
  elseif ($op === 'reject' && $selected) {
    ai_content_audit_update_candidate_status($selected, 'rejected');
    backdrop_set_message(t('Rejected @count item(s).', ['@count' => count($selected)]));
  }

  $form_state['redirect'] = 'admin/config/ai/content-audit/queue';
}

/**
 * Settings form.
 */
function ai_content_audit_settings_form($form, &$form_state) {
  $config = config('ai_content_audit.settings');

  $form['#config'] = 'ai_content_audit.settings';

  $traffic_uploaded = $config->get('traffic_uploaded');
  $traffic_count    = $config->get('traffic_path_count');

  $form['traffic'] = [
    '#type' => 'fieldset',
    '#title' => t('Traffic data (GA4 CSV)'),
    '#description' => t('In GA4 go to Reports → Engagement → Pages and screens, set your date range, and click the download icon to export CSV. Upload it here to populate traffic scores in cluster rankings.'),
  ];
  if ($traffic_uploaded) {
    $form['traffic']['current'] = [
      '#markup' => '<p>' . t('<strong>Current data:</strong> @count path(s) uploaded on @date. Upload a new file to replace it.', [
        '@count' => $traffic_count,
        '@date'  => format_date($traffic_uploaded, 'medium'),
      ]) . '</p>',
    ];
  }
  $form['traffic']['ga4_csv'] = [
    '#type' => 'file',
    '#title' => t('GA4 Pages and screens CSV'),
    '#description' => t('Accepted columns: "Page path and screen class" and "Views" (or "Sessions"). Other columns are ignored.'),
  ];

  $form['assistant'] = [
    '#type' => 'fieldset',
    '#title' => t('Assistant'),
    '#description' => t('When an assistant is configured, cron sends it a trigger prompt and it uses the audit tools to scan and propose actions. Configure assistants at <a href="@url">admin/config/ai/assistants</a>.', ['@url' => url('admin/config/ai/assistants')]),
  ];
  $assistant_options = ['' => t('— None (use direct PHP scanner) —')];
  if (module_exists('ai_assistant')) {
    $assistant_options += ai_assistant_options();
  }
  $form['assistant']['assistant_id'] = [
    '#type' => 'select',
    '#title' => t('Content audit assistant'),
    '#options' => $assistant_options,
    '#default_value' => $config->get('assistant_id') ?: '',
  ];
  $form['assistant']['assistant_prompt'] = [
    '#type' => 'textarea',
    '#title' => t('Trigger prompt'),
    '#description' => t('Sent to the assistant on each cron run.'),
    '#default_value' => $config->get('assistant_prompt') ?: 'Run a content audit. Use audit_trigger_scan to inventory content. For each top cluster: use audit_get_cluster to see candidates and their lifecycle analysis, use audit_get_node_content to read the actual content (including any ai_content_lifecycle findings), use audit_classify_rot to set the ROT classification (Redundant/Outdated/Trivial), then either use audit_propose_action to recommend an action or audit_propose_edit to draft specific text changes. Lifecycle ai_analysis fields explain what is wrong — use that context when proposing edits.',
    '#rows' => 4,
  ];

  $form['scan'] = [
    '#type' => 'fieldset',
    '#title' => t('Direct scanner settings'),
    '#description' => t('Used when no assistant is configured.'),
  ];
  $form['scan']['cron_enabled'] = [
    '#type' => 'checkbox',
    '#title' => t('Enable automatic cron scanning'),
    '#default_value' => $config->get('cron_enabled') ?: 0,
  ];
  $form['scan']['cron_interval'] = [
    '#type' => 'select',
    '#title' => t('How often to start a new scan'),
    '#description' => t('Once a scan completes, wait this long before starting the next one.'),
    '#options' => [
      3600   => t('Hourly'),
      21600  => t('Every 6 hours'),
      86400  => t('Daily'),
      604800 => t('Weekly'),
    ],
    '#default_value' => $config->get('cron_interval') ?: 86400,
  ];
  $form['scan']['cron_batch_size'] = [
    '#type' => 'select',
    '#title' => t('Nodes per cron batch'),
    '#description' => t('How many nodes to similarity-search per cron run. Lower = safer on shared hosting; higher = faster scans.'),
    '#options' => [10 => 10, 25 => 25, 50 => 50, 100 => 100, 200 => 200],
    '#default_value' => $config->get('cron_batch_size') ?: 50,
  ];
  $form['scan']['default_profile'] = [
    '#type' => 'textfield',
    '#title' => t('Default profile'),
    '#default_value' => $config->get('default_profile') ?: 'default',
    '#size' => 32,
  ];

  $form['similarity'] = [
    '#type' => 'fieldset',
    '#title' => t('Similarity settings'),
  ];
  $form['similarity']['similarity_threshold'] = [
    '#type' => 'textfield',
    '#title' => t('Similarity threshold (0.0–1.0)'),
    '#description' => t('Minimum vector similarity score to consider two pages related. 0.75 is a good starting point.'),
    '#default_value' => $config->get('similarity_threshold') ?: '0.75',
    '#size' => 8,
  ];

  $index_options = _ai_content_audit_index_options();
  $form['similarity']['search_api_index'] = [
    '#type' => 'select',
    '#title' => t('Search API AI index'),
    '#description' => t('Which vector index to use for semantic clustering. Leave empty to search all.'),
    '#options' => ['' => t('— All available indexes —')] + $index_options,
    '#default_value' => $config->get('search_api_index') ?: '',
  ];

  $form['content'] = [
    '#type' => 'fieldset',
    '#title' => t('Content scope'),
  ];
  $form['content']['bundle_filter_mode'] = [
    '#type' => 'radios',
    '#title' => t('Content type filter mode'),
    '#options' => [
      'exclude' => t('Scan all types <strong>except</strong> the selected'),
      'include' => t('Scan <strong>only</strong> the selected types'),
    ],
    '#default_value' => $config->get('bundle_filter_mode') ?: 'exclude',
  ];
  $bundles = node_type_get_types();
  $bundle_options = [];
  foreach ($bundles as $key => $type) {
    $bundle_options[$key] = $type->name;
  }
  $form['content']['excluded_bundles'] = [
    '#type' => 'checkboxes',
    '#title' => t('Content types'),
    '#description' => t('Which content types to exclude or include, depending on the mode above.'),
    '#options' => $bundle_options,
    '#default_value' => $config->get('excluded_bundles') ?: [],
  ];

  $form['llm'] = [
    '#type' => 'fieldset',
    '#title' => t('LLM summaries'),
  ];
  $form['llm']['llm_summaries_enabled'] = [
    '#type' => 'checkbox',
    '#title' => t('Generate LLM summaries for clusters'),
    '#description' => t('After clustering, call the LLM to write a 2-sentence summary for each cluster. Uses API credits.'),
    '#default_value' => $config->get('llm_summaries_enabled') ?: 0,
  ];

  $chat_models = function_exists('ai_get_all_chat_models') ? ai_get_all_chat_models() : [];
  $saved_model = $config->get('summary_model') ?: '';
  $default_model = function_exists('ai_default_model')
    ? ai_default_model($chat_models, $saved_model, ['openai/gpt-4o-mini', 'openai/gpt-4o'])
    : $saved_model;

  $form['llm']['summary_model'] = [
    '#type' => 'select',
    '#title' => t('Summary model'),
    '#description' => t('Model used to generate 2-sentence cluster summaries. Only chat models from enabled providers are listed.'),
    '#options' => ['' => t('— Select a model —')] + $chat_models,
    '#default_value' => $default_model,
    '#empty_option' => t('— Select a model —'),
  ];

  $saved_edit_model = $config->get('auto_draft_edit_model') ?: '';
  $default_edit_model = function_exists('ai_default_model')
    ? ai_default_model($chat_models, $saved_edit_model, [$default_model, 'openai/gpt-4o-mini', 'openai/gpt-4o'])
    : $saved_edit_model;

  $form['llm']['auto_draft_edits_enabled'] = [
    '#type' => 'checkbox',
    '#title' => t('Automatically draft canonical-page edits after scan'),
    '#description' => t('For pending clusters whose recommendation is merge or refresh, ask the AI to draft an update on the canonical page and save it as an approval-gated draft revision.'),
    '#default_value' => $config->get('auto_draft_edits_enabled') ?: 0,
  ];

  $form['llm']['auto_draft_edit_model'] = [
    '#type' => 'select',
    '#title' => t('Draft edit model'),
    '#description' => t('Model used to generate draft revisions for canonical pages.'),
    '#options' => ['' => t('— Select a model —')] + $chat_models,
    '#default_value' => $default_edit_model,
    '#empty_option' => t('— Select a model —'),
    '#states' => [
      'visible' => [
        ':input[name="auto_draft_edits_enabled"]' => ['checked' => TRUE],
      ],
    ],
  ];

  $form['llm']['auto_draft_edit_limit'] = [
    '#type' => 'select',
    '#title' => t('Draft edit limit per scan'),
    '#description' => t('Maximum number of canonical-page draft revisions to create after a completed scan.'),
    '#options' => [1 => 1, 3 => 3, 5 => 5, 10 => 10, 20 => 20],
    '#default_value' => $config->get('auto_draft_edit_limit') ?: 5,
    '#states' => [
      'visible' => [
        ':input[name="auto_draft_edits_enabled"]' => ['checked' => TRUE],
      ],
    ],
  ];

  $form['#attributes']['enctype'] = 'multipart/form-data';
  $form = system_settings_form($form);
  $form['#submit'] = ['ai_content_audit_settings_form_submit'];
  return $form;
}

/**
 * Settings form submit — saves config and processes GA4 CSV upload.
 */
function ai_content_audit_settings_form_submit($form, &$form_state) {
  // Handle file upload before saving config.
  $tmp = isset($_FILES['files']['tmp_name']['ga4_csv']) ? $_FILES['files']['tmp_name']['ga4_csv'] : '';
  if (!empty($tmp) && is_uploaded_file($tmp)) {
    $parsed = _ai_content_audit_parse_ga4_csv($tmp);
    if (!empty($parsed)) {
      db_truncate('ai_content_audit_traffic')->execute();
      $now = REQUEST_TIME;
      foreach ($parsed as $path => $views) {
        db_insert('ai_content_audit_traffic')->fields([
          'path'      => $path,
          'pageviews'  => (int) $views,
          'uploaded'  => $now,
        ])->execute();
      }
      $config = config('ai_content_audit.settings');
      $config->set('traffic_uploaded', $now);
      $config->set('traffic_path_count', count($parsed));
      $config->save();
      backdrop_set_message(t('Traffic data uploaded: @count path(s) imported.', ['@count' => count($parsed)]));
    }
    else {
      backdrop_set_message(t('Could not parse GA4 CSV — check file format.'), 'warning');
    }
  }

  // Let system_settings_form save remaining config values.
  system_settings_form_submit($form, $form_state);
}

/**
 * Parse a GA4 "Pages and screens" CSV export.
 * Returns array of path => pageviews. Skips metadata header rows.
 */
function _ai_content_audit_parse_ga4_csv($filepath) {
  $handle = @fopen($filepath, 'r');
  if (!$handle) {
    return [];
  }

  // Strip UTF-8 BOM if present.
  $bom = fread($handle, 3);
  if ($bom !== "\xEF\xBB\xBF") {
    rewind($handle);
  }

  $results     = [];
  $header      = NULL;
  $path_col    = NULL;
  $views_col   = NULL;

  while (($row = fgetcsv($handle)) !== FALSE) {
    if (empty($row) || (count($row) === 1 && trim($row[0]) === '')) {
      // Blank line — if we already started reading data, stop.
      if ($header !== NULL) {
        break;
      }
      continue;
    }

    // Skip GA4 metadata rows (start with '#').
    if ($header === NULL) {
      if (isset($row[0]) && strncmp(ltrim($row[0]), '#', 1) === 0) {
        continue;
      }
      // Detect header row: contains "Page path" in first column AND has multiple columns.
      if (count($row) > 1 && stripos(trim($row[0]), 'Page path') !== FALSE) {
        $header = array_map('trim', $row);
        foreach ($header as $idx => $col) {
          if (stripos($col, 'Page path') !== FALSE) {
            $path_col = $idx;
          }
          if (strcasecmp($col, 'Views') === 0 || strcasecmp($col, 'Sessions') === 0
              || stripos($col, 'pageview') !== FALSE || stripos($col, 'page view') !== FALSE) {
            $views_col = $idx;
          }
        }
      }
      continue;
    }

    // Data row.
    if ($path_col === NULL || $views_col === NULL) {
      continue;
    }
    $path  = isset($row[$path_col]) ? trim($row[$path_col]) : '';
    $views = isset($row[$views_col]) ? (int) str_replace(',', '', trim($row[$views_col])) : 0;

    if ($path === '' || $path === '(not set)' || $views <= 0) {
      continue;
    }
    // Ensure leading slash.
    if ($path[0] !== '/') {
      $path = '/' . $path;
    }
    $results[$path] = $views;
  }

  fclose($handle);
  return $results;
}

/**
 * Helper: get available Search API index options.
 */
function _ai_content_audit_index_options() {
  $options = [];
  if (!function_exists('search_api_index_load_multiple')) {
    return $options;
  }
  $indexes = search_api_index_load_multiple(FALSE);
  foreach ($indexes as $index) {
    if (!empty($index->machine_name)) {
      $options[$index->machine_name] = !empty($index->name) ? $index->name : $index->machine_name;
    }
  }
  return $options;
}

/**
 * Focused text-diff page for a proposed edit.
 *
 * Compares raw editable-field source (title + paragraph/field body) between
 * the original node revision and the AI-proposed one. Avoids the Diff
 * module's full-render comparison, which gets dominated by paragraph wrapper
 * and admin dropbutton markup.
 */
function ai_content_audit_proposed_edit_diff_page($acid) {
  $acid = (int) $acid;
  $candidate = db_query(
    'SELECT * FROM {ai_content_audit_candidate} WHERE acid = :acid',
    [':acid' => $acid]
  )->fetchAssoc();
  if (!$candidate) {
    backdrop_set_message(t('Candidate @acid not found.', ['@acid' => $acid]), 'error');
    backdrop_goto('admin/config/ai/content-audit/queue');
  }

  $state = !empty($candidate['before_state']) ? json_decode($candidate['before_state'], TRUE) : [];
  $proposed_vid = !empty($state['proposed_vid']) ? (int) $state['proposed_vid'] : 0;
  $current_vid = !empty($state['current_vid']) ? (int) $state['current_vid'] : 0;
  $nid = (int) $candidate['nid'];

  if (!$proposed_vid || !$nid) {
    return ['#markup' => '<p>' . t('No proposed revision recorded for this candidate.') . '</p>'];
  }

  $original_node = node_load($nid, $current_vid ?: NULL);
  $proposed_node = node_load($nid, $proposed_vid);
  if (!$original_node || !$proposed_node) {
    return ['#markup' => '<p>' . t('Could not load both revisions — the proposed draft may have been deleted.') . '</p>'];
  }

  // Resolve the actual editable text for both sides. For paragraph-backed
  // bodies, the node fields reference paragraph revisions; load those and
  // read the raw value rather than rendering the node.
  $target_context = !empty($state['target_context']) && is_array($state['target_context']) ? $state['target_context'] : [];
  $original_body = '';
  $proposed_body = '';
  $field_label = !empty($state['target_field']) ? $state['target_field'] : 'body';

  $extract_text_field_value = function ($entity, $field_name) {
    if (!$entity || !isset($entity->{$field_name}) || !is_array($entity->{$field_name})) {
      return '';
    }
    $parts = [];
    foreach ($entity->{$field_name} as $lang_items) {
      if (!is_array($lang_items)) {
        continue;
      }
      foreach ($lang_items as $entry) {
        if (is_array($entry) && !empty($entry['value'])) {
          $parts[] = $entry['value'];
        }
      }
    }
    return implode("\n\n", $parts);
  };

  $diag = [];

  if (!empty($target_context['target_type']) && $target_context['target_type'] === 'paragraph_field' && module_exists('paragraphs')) {
    $original_paragraph_rev = !empty($state['paragraph_previous_revision_id'])
      ? (int) $state['paragraph_previous_revision_id']
      : (!empty($target_context['revision_id']) ? (int) $target_context['revision_id'] : 0);
    $proposed_paragraph_rev = !empty($state['paragraph_proposed_revision_id'])
      ? (int) $state['paragraph_proposed_revision_id']
      : 0;

    // Legacy rows without paragraph_proposed_revision_id: pick the smallest
    // paragraph revision newer than the original.
    if (!$proposed_paragraph_rev && !empty($target_context['item_id']) && $original_paragraph_rev) {
      $proposed_paragraph_rev = (int) db_query(
        'SELECT MIN(revision_id) FROM {paragraphs_item_revision} WHERE item_id = :id AND revision_id > :prev',
        [':id' => (int) $target_context['item_id'], ':prev' => $original_paragraph_rev]
      )->fetchField();
    }

    // If we still don't have a proposed revision and the paragraph has more
    // than one revision, fall back to its current default revision.
    if (!$proposed_paragraph_rev && !empty($target_context['item_id'])) {
      $current_default = (int) db_query(
        'SELECT revision_id FROM {paragraphs_item} WHERE item_id = :id',
        [':id' => (int) $target_context['item_id']]
      )->fetchField();
      if ($current_default && $current_default !== $original_paragraph_rev) {
        $proposed_paragraph_rev = $current_default;
      }
    }

    $field_name = $target_context['field_name'];
    if ($original_paragraph_rev) {
      $p = paragraphs_item_revision_load($original_paragraph_rev);
      $original_body = $extract_text_field_value($p, $field_name);
    }
    if ($proposed_paragraph_rev) {
      $p = paragraphs_item_revision_load($proposed_paragraph_rev);
      $proposed_body = $extract_text_field_value($p, $field_name);
    }

    $field_label = $target_context['paragraph_bundle'] . '.' . $field_name . ' (paragraph #' . (!empty($target_context['item_id']) ? (int) $target_context['item_id'] : '?') . ')';
    $diag['paragraph_item_id'] = !empty($target_context['item_id']) ? (int) $target_context['item_id'] : 0;
    $diag['original_paragraph_rev'] = $original_paragraph_rev;
    $diag['proposed_paragraph_rev'] = $proposed_paragraph_rev;
    $diag['original_body_len'] = strlen($original_body);
    $diag['proposed_body_len'] = strlen($proposed_body);
  }
  else {
    $field_name = !empty($state['target_field']) ? $state['target_field'] : 'body';
    $original_body = $extract_text_field_value($original_node, $field_name);
    $proposed_body = $extract_text_field_value($proposed_node, $field_name);
    $diag['target_field'] = $field_name;
    $diag['original_body_len'] = strlen($original_body);
    $diag['proposed_body_len'] = strlen($proposed_body);
  }

  $reason = !empty($candidate['proposed_action_detail']) ? $candidate['proposed_action_detail'] : '';

  $out = [];
  $out['header'] = [
    '#markup' => '<h2>' . check_plain($candidate['title']) . '</h2>'
      . '<p><a href="' . url('node/' . $nid) . '" target="_blank">' . t('View live node') . '</a> · '
      . '<a href="' . url('admin/config/ai/content-audit/queue') . '">' . t('Back to queue') . '</a></p>',
  ];

  if ($reason !== '') {
    $out['reason'] = [
      '#markup' => '<p><strong>' . t('AI reason:') . '</strong> ' . check_plain($reason) . '</p>',
    ];
  }

  $diag_lines = [t('Comparing field: @f', ['@f' => $field_label])];
  $diag_lines[] = t('Node vid @c → @p', ['@c' => $current_vid ?: '(node default)', '@p' => $proposed_vid]);
  foreach ($diag as $k => $v) {
    $diag_lines[] = check_plain($k) . ' = ' . check_plain((string) $v);
  }
  $out['meta'] = [
    '#markup' => '<p style="color:#666;font-size:0.9em">' . implode(' · ', $diag_lines) . '</p>',
  ];

  // Show the raw extracted source on both sides so you can see what was fed
  // into the diff (useful when the diff comes up empty).
  $out['raw'] = [
    '#markup' => '<details><summary>' . t('Raw extracted text (debug)') . '</summary>'
      . '<h4>' . t('Original (raw value)') . '</h4>'
      . '<pre style="white-space:pre-wrap;background:#f5f5f5;padding:.5em;max-height:300px;overflow:auto">' . check_plain($original_body) . '</pre>'
      . '<h4>' . t('Proposed (raw value)') . '</h4>'
      . '<pre style="white-space:pre-wrap;background:#f5f5f5;padding:.5em;max-height:300px;overflow:auto">' . check_plain($proposed_body) . '</pre>'
      . '</details>',
  ];

  // Title diff.
  if ($original_node->title !== $proposed_node->title) {
    $out['title'] = [
      '#markup' => '<h3>' . t('Title') . '</h3>'
        . '<div style="padding:.5em;background:#fee;text-decoration:line-through;margin-bottom:.25em">' . check_plain($original_node->title) . '</div>'
        . '<div style="padding:.5em;background:#efe">' . check_plain($proposed_node->title) . '</div>',
    ];
  }

  // Body diff. Strip tags so the inline diff focuses on words, not markup.
  $original_text = trim(html_entity_decode(strip_tags(preg_replace('/<\s*br\s*\/?\s*>/i', "\n", $original_body)), ENT_QUOTES, 'UTF-8'));
  $proposed_text = trim(html_entity_decode(strip_tags(preg_replace('/<\s*br\s*\/?\s*>/i', "\n", $proposed_body)), ENT_QUOTES, 'UTF-8'));

  if ($original_text === '' && $proposed_text === '') {
    $out['body_empty'] = ['#markup' => '<p>' . t('No body text on either side.') . '</p>'];
  }
  elseif ($original_text === $proposed_text) {
    $out['body_nochange'] = ['#markup' => '<p><em>' . t('Body text is identical after stripping markup — proposal contained only formatting/whitespace changes.') . '</em></p>'];
  }
  else {
    $out['body_heading'] = ['#markup' => '<h3>' . t('Body') . ' <span style="font-weight:normal;color:#666;font-size:.8em">' . t('(inline word diff, markup stripped)') . '</span></h3>'];
    if (module_exists('diff')) {
      module_load_include('inc', 'diff', 'includes/diff.backdrop_diff_inline');
      $renderer = new BackdropDiffInline($original_text, $proposed_text);
      $out['body_diff'] = [
        '#markup' => '<div class="diff-inline" style="padding:1em;border:1px solid #ccc;background:#fafafa;white-space:pre-wrap;font-family:inherit;line-height:1.5">' . $renderer->render() . '</div>',
        '#attached' => [
          'css' => [
            backdrop_get_path('module', 'diff') . '/css/diff.default.css',
          ],
        ],
      ];
      // Inline style overrides — Diff's default CSS is for table view and the
      // inline highlight selectors may not be obvious without explicit rules.
      $out['body_diff_css'] = [
        '#markup' => '<style>'
          . '.diff-inline .diff-added,.diff-inline ins{background:#dfd;text-decoration:none;padding:0 2px}'
          . '.diff-inline .diff-deleted,.diff-inline del{background:#fdd;text-decoration:line-through;padding:0 2px}'
          . '.diff-inline .diff-changed{background:#ffd;padding:0 2px}'
          . '</style>',
      ];
    }
    else {
      $out['body_diff'] = [
        '#markup' => '<h4>' . t('Original') . '</h4><pre style="white-space:pre-wrap;background:#fee;padding:.5em">' . check_plain($original_text) . '</pre>'
          . '<h4>' . t('Proposed') . '</h4><pre style="white-space:pre-wrap;background:#efe;padding:.5em">' . check_plain($proposed_text) . '</pre>',
      ];
    }
  }

  return $out;
}
