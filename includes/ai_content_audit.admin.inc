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
 *   - ai_content_audit_settings_form     — Settings: exact matching, cron,
 *                                           LLM model, auto-draft, GA4 traffic
 *                                           CSV upload, bundle filter.
 *
 * Also defines the proposed-edit diff page callback
 * (ai_content_audit_proposed_edit_diff_page) for the per-candidate text diff.
 */

/**
 * Dashboard form.
 */
function ai_content_audit_dashboard_form($form, &$form_state) {
  $counts = ai_content_audit_cluster_counts();
  $rot_counts = ai_content_audit_rot_counts();
  $latest_run = db_query('SELECT * FROM {ai_content_audit_run} ORDER BY started DESC LIMIT 1')->fetchAssoc();

  $form['intro'] = [
    '#markup' => '<p>' . t('Finds exact duplicate content sets. Produces ranked reports for human review before any changes are applied.') . '</p>',
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
    '#header' => [t('Review State'), t('Duplicate Sets')],
    '#rows' => [
      [t('Pending'), isset($counts['pending']) ? $counts['pending'] : 0],
      [t('Dismissed'), isset($counts['dismissed']) ? $counts['dismissed'] : 0],
      [t('Actioned'), isset($counts['actioned']) ? $counts['actioned'] : 0],
    ],
    '#empty' => t('No exact duplicate sets in the current scan.'),
  ];

  $form['rot_counts'] = [
    '#theme' => 'table',
    '#caption' => t('ROT classifications'),
    '#header' => [t('Classification'), t('Pages')],
    '#rows' => [
      [l(t('Redundant'), 'admin/config/ai/content-audit/rot', ['query' => ['rot' => 'redundant']]), isset($rot_counts['redundant']) ? $rot_counts['redundant'] : 0],
      [l(t('Redundant + Outdated'), 'admin/config/ai/content-audit/rot', ['query' => ['rot' => 'redundant_outdated']]), isset($rot_counts['redundant_outdated']) ? $rot_counts['redundant_outdated'] : 0],
      [l(t('Outdated'), 'admin/config/ai/content-audit/rot', ['query' => ['rot' => 'outdated']]), isset($rot_counts['outdated']) ? $rot_counts['outdated'] : 0],
      [l(t('Trivial'), 'admin/config/ai/content-audit/rot', ['query' => ['rot' => 'trivial']]), isset($rot_counts['trivial']) ? $rot_counts['trivial'] : 0],
      [l(t('Review'), 'admin/config/ai/content-audit/rot', ['query' => ['rot' => 'review']]), isset($rot_counts['review']) ? $rot_counts['review'] : 0],
    ],
    '#empty' => t('No ROT scores yet. Run a scan.'),
  ];
  $form['rot_guide'] = [
    '#markup' => ai_content_audit_rot_guide_markup(),
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

  $latest_cluster_run = ai_content_audit_latest_cluster_run_id();
  $top_rot = $latest_cluster_run && db_table_exists('ai_content_audit_node_score')
    ? db_query(
      'SELECT * FROM {ai_content_audit_node_score}
        WHERE run_id = :run_id
        ORDER BY composite_score DESC, age_score DESC, nid ASC
        LIMIT 10',
      [':run_id' => $latest_cluster_run]
    )->fetchAll(PDO::FETCH_ASSOC)
    : [];
  $rows = [];
  foreach ($top_rot as $score) {
    $rows[] = [
      l(check_plain($score['title']) . ' #' . (int) $score['nid'], 'admin/config/ai/content-audit/rot/' . (int) $score['sid']),
      check_plain($score['rot_type']),
      round($score['composite_score'], 3),
      check_plain($score['recommended_action']),
    ];
  }

  $form['top_clusters'] = [
    '#theme' => 'table',
    '#caption' => t('Top ROT pages by priority'),
    '#header' => [t('Page'), t('ROT'), t('Priority'), t('Recommendation')],
    '#rows' => $rows,
    '#empty' => t('No ROT scores yet.'),
  ];

  return $form;
}

/**
 * Explain the ROT classifications and score columns in administrator-facing UI.
 */
function ai_content_audit_rot_guide_markup() {
  $definitions = [
    [t('Redundant'), t('High-similarity pages that may cover the same content. Semantic matches stay review-only; exact matches appear in Duplicates.')],
    [t('Redundant + Outdated'), t('High-similarity pages where the content is also stale according to the configured staleness curve.')],
    [t('Outdated'), t('Content whose last update is old enough to need a freshness review, without enough similarity evidence to call it redundant.')],
    [t('Trivial'), t('Short, low-substance content that is not sufficiently similar to another page.')],
    [t('Review'), t('No decisive ROT signal. An editor should inspect the page and decide whether to keep, refresh, merge, or retire it.')],
  ];
  $items = '';
  foreach ($definitions as $definition) {
    $items .= '<dt>' . check_plain($definition[0]) . '</dt><dd>' . check_plain($definition[1]) . '</dd>';
  }

  return '<details class="ai-content-audit-rot-guide" style="margin:1em 0">'
    . '<summary>' . t('What do the ROT classifications mean?') . '</summary>'
    . '<dl>' . $items . '</dl>'
    . '<p><strong>' . t('Node age') . ':</strong> ' . t('Time since the node was created, shown in months.') . '<br>'
    . '<strong>' . t('Last updated') . ':</strong> ' . t('Time since the node was last changed, shown in months.') . '<br>'
    . '<strong>' . t('Staleness') . ':</strong> ' . t('A normalized 0–1 urgency score used by classification and priority; it is not the page age itself.') . '<br>'
    . '<strong>' . t('Similarity') . ':</strong> ' . t('The strongest available content match from the configured search index.') . '</p>'
    . '<p><strong>' . t('Retire recommendation') . ':</strong> ' . t('Created at least 10 years ago, fully stale, very low traffic, and not flagged as a duplicate. Retirement always requires administrator approval.') . '</p>'
    . '</details>';
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
      $cluster_count = (int) db_query(
        'SELECT cluster_count FROM {ai_content_audit_run} WHERE run_id = :rid',
        [':rid' => $active['run_id']]
      )->fetchField();
      backdrop_set_message(t('Scan complete: @nodes node(s) processed, @clusters cluster(s) found.', [
        '@nodes'    => $result['total'],
        '@clusters' => $cluster_count,
      ]));
    }
    elseif (module_exists('ai_async')) {
      // Hand the remaining batches to the self-chaining async callback so the
      // scan finishes in the background without further clicks or cron ticks.
      ai_async_submit('ai_content_audit_async_scan_batch', [
        'run_id'     => $active['run_id'],
        'batch_size' => $batch_size,
      ]);
      backdrop_set_message(t('Batch processed: @done of @total node(s). The remaining batches will continue in the background.', [
        '@done'  => $result['processed'],
        '@total' => $result['total'],
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
 * Per-node ROT score list.
 */
function ai_content_audit_rot_scores_form($form, &$form_state) {
  $rot_filter = !empty($_GET['rot']) ? $_GET['rot'] : 'all';
  $recommendation_filter = !empty($_GET['recommendation']) ? $_GET['recommendation'] : 'all';
  $sort_key = !empty($_GET['rot_sort']) ? $_GET['rot_sort'] : 'priority';
  $sort_direction = !empty($_GET['rot_dir']) ? strtolower($_GET['rot_dir']) : 'desc';
  $sort_options = [
    'priority' => t('Priority'),
    'recommended_action' => t('Recommendation'),
    'similarity_score' => t('Similarity'),
    'node_age_months' => t('Node age'),
    'age_months' => t('Last updated'),
    'age_score' => t('Staleness'),
    'traffic_score' => t('Traffic'),
    'title' => t('Page'),
  ];
  $sort_fields = [
    'priority' => 's.composite_score',
    'recommended_action' => 's.recommended_action',
    'similarity_score' => 's.similarity_score',
    'node_age_months' => 's.node_age_months',
    'age_months' => 's.age_months',
    'age_score' => 's.age_score',
    'traffic_score' => 's.traffic_score',
    'title' => 's.title',
  ];
  if (!isset($sort_fields[$sort_key])) {
    $sort_key = 'priority';
  }
  if (!in_array($sort_direction, ['asc', 'desc'], TRUE)) {
    $sort_direction = 'desc';
  }
  $run_id = ai_content_audit_latest_cluster_run_id();

  $form['rot_guide'] = [
    '#markup' => ai_content_audit_rot_guide_markup(),
  ];

  $form['filter'] = [
    '#type' => 'select',
    '#title' => t('ROT classification'),
    '#options' => [
      'all' => t('All pages'),
      'redundant' => t('Redundant'),
      'redundant_outdated' => t('Redundant + Outdated'),
      'outdated' => t('Outdated'),
      'trivial' => t('Trivial'),
      'review' => t('Review'),
    ],
    '#default_value' => $rot_filter,
    '#attributes' => ['onchange' => 'this.form.submit()'],
  ];
  $form['filter_submit'] = [
    '#type' => 'submit',
    '#value' => t('Filter'),
    '#attributes' => ['class' => ['element-invisible']],
  ];
  $form['recommendation'] = [
    '#type' => 'select',
    '#title' => t('Recommendation'),
    '#options' => [
      'all' => t('All recommendations'),
      'retire' => t('Retire'),
      'refresh' => t('Refresh'),
      'prune' => t('Prune'),
      'merge' => t('Merge'),
      'review' => t('Review'),
    ],
    '#default_value' => $recommendation_filter,
    '#attributes' => ['onchange' => 'this.form.submit()'],
  ];
  $form['sort'] = [
    '#type' => 'select',
    '#title' => t('Sort by'),
    '#options' => $sort_options,
    '#default_value' => $sort_key,
    '#attributes' => ['onchange' => 'this.form.submit()'],
  ];
  $form['sort_direction'] = [
    '#type' => 'select',
    '#title' => t('Order'),
    '#options' => [
      'desc' => t('Highest / Z-A first'),
      'asc' => t('Lowest / A-Z first'),
    ],
    '#default_value' => $sort_direction,
    '#attributes' => ['onchange' => 'this.form.submit()'],
  ];
  $form['#submit'][] = 'ai_content_audit_rot_scores_form_submit';

  $header = [
    ['data' => t('Page'), 'field' => 's.title'],
    ['data' => t('Type'), 'field' => 's.bundle'],
    ['data' => t('ROT'), 'field' => 's.rot_type'],
    ['data' => t('Similarity'), 'field' => 's.similarity_score'],
    ['data' => t('Node age (months)'), 'field' => 's.node_age_months'],
    ['data' => t('Last updated (months)'), 'field' => 's.age_months'],
    ['data' => t('Staleness'), 'field' => 's.age_score'],
    ['data' => t('Traffic'), 'field' => 's.traffic_score'],
    ['data' => t('Priority'), 'field' => 's.composite_score'],
    ['data' => t('Recommendation'), 'field' => 's.recommended_action'],
    ['data' => t('Status'), 'field' => 's.review_state'],
  ];
  $rows = [];
  if ($run_id && db_table_exists('ai_content_audit_node_score')) {
    $query = db_select('ai_content_audit_node_score', 's')
      ->fields('s')
      ->condition('s.run_id', $run_id)
      ->orderBy($sort_fields[$sort_key], $sort_direction)
      ->orderBy($sort_key === 'title' ? 's.nid' : 's.title', 'ASC')
      ->range(0, 1000);
    if ($rot_filter !== 'all') {
      $query->condition('s.rot_type', $rot_filter);
    }
    if ($recommendation_filter !== 'all') {
      $query->condition('s.recommended_action', $recommendation_filter);
    }
    foreach ($query->execute() as $score) {
      $rows[] = [
        l(check_plain($score->title) . ' #' . (int) $score->nid, 'admin/config/ai/content-audit/rot/' . (int) $score->sid),
        check_plain($score->bundle),
        check_plain($score->rot_type),
        round($score->similarity_score, 3),
        round($score->node_age_months, 1),
        round($score->age_months, 1),
        round($score->age_score, 3),
        round($score->traffic_score, 3),
        round($score->composite_score, 3),
        check_plain($score->recommended_action),
        check_plain($score->review_state),
      ];
    }
  }

  $form['context'] = [
    '#markup' => $run_id
      ? '<p class="description">' . t('Every published page receives a ROT classification from the newest completed scan. Exact duplicate membership is shown separately on the Duplicates tab.') . '</p>'
      : '<p class="description">' . t('No completed scan results are available yet.') . '</p>',
  ];
  $form['scores'] = [
    '#theme' => 'table',
    '#header' => $header,
    '#rows' => $rows,
    '#empty' => t('No pages match this ROT filter.'),
  ];
  return $form;
}

/**
 * Detail form for one per-node ROT assessment.
 */
function ai_content_audit_rot_detail_form($form, &$form_state, $sid) {
  $score = db_query(
    'SELECT * FROM {ai_content_audit_node_score} WHERE sid = :sid',
    [':sid' => (int) $sid]
  )->fetchAssoc();
  if (!$score) {
    backdrop_set_message(t('ROT assessment not found.'), 'error');
    backdrop_goto('admin/config/ai/content-audit/rot');
  }

  $node = node_load((int) $score['nid']);
  $merge_target = '';
  if (!empty($score['merge_into_nid'])) {
    $target_nid = (int) $score['merge_into_nid'];
    $target_title = db_query('SELECT title FROM {node} WHERE nid = :nid', [':nid' => $target_nid])->fetchField();
    $merge_target = '<dt>' . t('Merge into') . '</dt><dd>'
      . ($target_title !== FALSE ? l($target_title, 'node/' . $target_nid, ['attributes' => ['target' => '_blank']]) : t('Node @nid no longer exists.', ['@nid' => $target_nid]))
      . ' <em>(' . t('confirmed interchangeable by a decision model') . ')</em></dd>';
  }
  $form['summary'] = [
    '#markup' => '<h2>' . check_plain($score['title']) . '</h2>'
      . '<p>' . l(t('View published node'), 'node/' . (int) $score['nid'], ['attributes' => ['target' => '_blank']])
      . ' · ' . ($node ? l(t('Edit node'), 'node/' . (int) $score['nid'] . '/edit', ['attributes' => ['target' => '_blank']]) : t('Node no longer exists.')) . '</p>'
      . '<dl>'
      . '<dt>' . t('ROT classification') . '</dt><dd><strong>' . check_plain($score['rot_type']) . '</strong></dd>'
      . '<dt>' . t('Similarity score') . '</dt><dd>' . round($score['similarity_score'], 3) . '</dd>'
      . '<dt>' . t('Node age') . '</dt><dd>' . round($score['node_age_months'], 1) . ' ' . t('months') . '</dd>'
      . '<dt>' . t('Last updated') . '</dt><dd>' . round($score['age_months'], 1) . ' ' . t('months') . '</dd>'
      . '<dt>' . t('Staleness score') . '</dt><dd>' . round($score['age_score'], 3) . '</dd>'
      . '<dt>' . t('Traffic score') . '</dt><dd>' . round($score['traffic_score'], 3) . '</dd>'
      . '<dt>' . t('ROT priority') . '</dt><dd>' . round($score['composite_score'], 3) . '</dd>'
      . '<dt>' . t('Recommendation') . '</dt><dd>' . check_plain($score['recommended_action']) . '</dd>'
      . $merge_target
      . '<dt>' . t('Status') . '</dt><dd>' . check_plain($score['review_state']) . '</dd>'
      . '</dl>',
  ];
  $form['#rot_score'] = $score;
  $form['actions'] = ['#type' => 'actions'];
  $form['actions']['suggest_ai_edit'] = [
    '#type' => 'submit',
    '#value' => t('Suggest AI edit'),
    '#op' => 'suggest_ai_edit',
    '#access' => !in_array($score['rot_type'], ['redundant', 'redundant_outdated'], TRUE),
  ];
  $form['actions']['mark_editing'] = [
    '#type' => 'submit',
    '#value' => t('Mark for editing'),
    '#op' => 'mark_editing',
  ];
  $form['actions']['queue_prune'] = [
    '#type' => 'submit',
    '#value' => t('Queue prune for approval'),
    '#op' => 'queue_prune',
    '#access' => $score['recommended_action'] === 'prune',
  ];
  $form['actions']['queue_merge'] = [
    '#type' => 'submit',
    '#value' => t('Queue merge for approval'),
    '#op' => 'queue_merge',
    '#access' => $score['recommended_action'] === 'merge',
  ];
  $form['actions']['retire'] = [
    '#type' => 'submit',
    '#value' => t('Queue retire for approval'),
    '#op' => 'retire',
    '#access' => $score['recommended_action'] === 'retire',
  ];
  $form['actions']['keep_review'] = [
    '#type' => 'submit',
    '#value' => t('Keep for review'),
    '#op' => 'keep_review',
  ];
  $form['actions']['ignore'] = [
    '#type' => 'submit',
    '#value' => t('Ignore'),
    '#op' => 'ignore',
  ];
  $form['actions']['reset'] = [
    '#type' => 'submit',
    '#value' => t('Reset status'),
    '#op' => 'reset',
  ];
  return $form;
}

/**
 * Save a ROT assessment review decision.
 */
function ai_content_audit_rot_detail_form_submit($form, &$form_state) {
  $score = $form['#rot_score'];
  $op = !empty($form_state['triggering_element']['#op']) ? $form_state['triggering_element']['#op'] : '';

  if ($op === 'suggest_ai_edit') {
    module_load_include('inc', 'ai_content_audit', 'includes/ai_content_audit.llm');
    $result = ai_content_audit_auto_draft_node_edit($score);
    if (!empty($result['error'])) {
      backdrop_set_message(t('No AI draft revision was created: @message', ['@message' => $result['error']]), 'warning');
    }
    else {
      db_update('ai_content_audit_node_score')
        ->fields(['review_state' => 'actioned', 'updated' => REQUEST_TIME])
        ->condition('sid', (int) $score['sid'])
        ->execute();
      backdrop_set_message(t('AI draft revision proposed. Review the diff and approve it from the Action Queue.'));
    }
    $form_state['redirect'] = 'admin/config/ai/content-audit/rot/' . (int) $score['sid'];
    return;
  }

  $state = 'pending';
  if ($op === 'mark_editing') {
    ai_content_audit_queue_rot_candidate($score);
    $state = 'actioned';
    backdrop_set_message(t('Page marked for editing and added to the Action Queue.'));
  }
  elseif ($op === 'retire') {
    ai_content_audit_queue_rot_candidate($score, 'retire');
    $state = 'actioned';
    backdrop_set_message(t('Retire action queued for approval in the Action Queue.'));
  }
  elseif ($op === 'queue_prune' || $op === 'queue_merge') {
    $task_type = $op === 'queue_prune' ? 'prune' : 'merge';
    ai_content_audit_queue_rot_candidate($score, $task_type);
    $state = 'actioned';
    backdrop_set_message(t('@action action queued for approval in the Action Queue.', ['@action' => ucfirst($task_type)]));
  }
  elseif ($op === 'ignore') {
    $state = 'dismissed';
  }
  elseif ($op === 'keep_review') {
    $state = 'pending';
  }
  db_update('ai_content_audit_node_score')
    ->fields(['review_state' => $state, 'updated' => REQUEST_TIME])
    ->condition('sid', (int) $score['sid'])
    ->execute();
  if ($op !== 'mark_editing') {
    backdrop_set_message(t('ROT assessment status updated.'));
  }
  $form_state['redirect'] = 'admin/config/ai/content-audit/rot';
}

/**
 * Preserve the ROT filter in the URL after the auto-submit.
 */
function ai_content_audit_rot_scores_form_submit($form, &$form_state) {
  $rot = !empty($form_state['values']['filter']) ? $form_state['values']['filter'] : 'all';
  $recommendation = !empty($form_state['values']['recommendation']) ? $form_state['values']['recommendation'] : 'all';
  $sort = !empty($form_state['values']['sort']) ? $form_state['values']['sort'] : 'priority';
  $direction = !empty($form_state['values']['sort_direction']) ? $form_state['values']['sort_direction'] : 'desc';
  $form_state['redirect'] = [
    'admin/config/ai/content-audit/rot',
    ['query' => ['rot' => $rot, 'recommendation' => $recommendation, 'rot_sort' => $sort, 'rot_dir' => $direction]],
  ];
}

/**
 * Clusters list form.
 */
function ai_content_audit_clusters_form($form, &$form_state) {
  $review_state = !empty($_GET['state']) ? $_GET['state'] : 'pending';
  $selected_run_id = !empty($_GET['run_id']) ? trim($_GET['run_id']) : ai_content_audit_latest_cluster_run_id();

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

  $form['run_context'] = [
    '#markup' => $selected_run_id
      ? '<p class="description">' . t('Showing results from the newest usable scan. Historical scan results remain available from the Runs tab.') . '</p>'
      : '<p class="description">' . t('No completed scan results are available yet.') . '</p>',
  ];

  $header = [
    ['data' => t('Topic'),      'field' => 'c.topic_label'],
    ['data' => t('Pages'),      'field' => 'c.source_count'],
    ['data' => t('ROT'),        'field' => 'c.rot_type'],
    ['data' => t('Similarity'), 'field' => 'c.similarity_score'],
    ['data' => t('Age'),        'field' => 'c.age_score'],
    ['data' => t('Traffic'),    'field' => 'c.traffic_score'],
    ['data' => t('Priority'),   'field' => 'c.composite_score', 'sort' => 'desc'],
    ['data' => t('Action'),     'field' => 'c.recommended_action'],
    ['data' => t('Scan date'),  'field' => 'r.started'],
  ];

  $query = db_select('ai_content_audit_cluster', 'c')->extend('TableSort');
  $query->leftJoin('ai_content_audit_run', 'r', 'r.run_id = c.run_id');
  $query->fields('c');
  $query->addField('r', 'started', 'run_started');
  $query->condition('c.review_state', $review_state);
  if ($selected_run_id) {
    $query->condition('c.run_id', $selected_run_id);
  }
  else {
    // Prevent a missing current run from falling back to historical rows.
    $query->condition('c.cid', 0);
  }
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
    '#markup' => '<details style="margin-bottom:1em"><summary>' . t('Priority &amp; ROT column guide') . '</summary><dl>'
      . '<dt>' . t('ROT') . '</dt><dd>' . t('<strong>Redundant</strong> = high-similarity content; semantic matches remain review-only. <strong>Outdated</strong> = old content without enough similarity evidence to call it redundant. <strong>Trivial</strong> = short, low-substance content. <strong>Redundant + Outdated</strong> = high similarity plus staleness.') . '</dd>'
      . '<dt>' . t('Similarity (0–1)') . '</dt><dd>' . t('Highest content-match score found for the page. Exact-content duplicate clusters score 1.0.') . '</dd>'
      . '<dt>' . t('Staleness (0–1)') . '</dt><dd>' . t('Normalized urgency based on last update. It is separate from the actual age shown on ROT Scores.') . '</dd>'
      . '<dt>' . t('Traffic (0–1)') . '</dt><dd>' . t('Normalized GA4 pageviews. 1.0 = highest-traffic cluster.') . '</dd>'
      . '<dt>' . t('Priority (0–1)') . '</dt><dd>' . t('Consolidation priority: Similarity + Age + Traffic. Higher = more urgent action needed.') . '</dd>'
      . '<dt>' . t('Recommendation') . '</dt><dd>' . t('<strong>prune</strong> = unpublish an exact duplicate; <strong>merge</strong> = send an exact duplicate for consolidation review; <strong>refresh</strong> = edit stale content; <strong>retire</strong> = unpublish a fully stale, very low-traffic legacy page after approval; <strong>review</strong> = human judgement needed.') . '</dd>'
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

  if (ai_content_audit_latest_cluster_run_id() !== (string) $cluster['run_id']) {
    $form['historical_note'] = [
      '#markup' => '<div class="messages warning"><strong>' . t('Historical scan result') . ':</strong> '
        . t('This cluster belongs to an older scan and is not part of the current Clusters view. Its labels are retained for history only.')
        . '</div>',
      '#weight' => -100,
    ];
  }

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

  module_load_include('inc', 'ai_content_audit', 'includes/ai_content_audit.scoring');
  $weights = ai_content_audit_get_priority_weights();
  $weights_label = t('Similarity @sim% + Age @age% + Traffic @traf%', [
    '@sim' => round($weights['similarity'] * 100),
    '@age' => round($weights['age'] * 100),
    '@traf' => round($weights['traffic'] * 100),
  ]);

  $form['meta'] = [
    '#markup' => '<div style="margin-bottom:1em">'
      . '<div style="margin-bottom:0.5em">' . $rot_badge . ' &nbsp; <em>' . $rot_description . '</em></div>'
      . '</div>'
      . '<dl>'
      . '<dt>' . t('Topic') . '</dt><dd>' . check_plain($cluster['topic_label']) . '</dd>'
      . '<dt>' . t('Recommended action') . '</dt><dd><strong>' . check_plain($cluster['recommended_action']) . '</strong>'
        . ' <small>(' . t('prune = near-duplicate; merge = overlapping; refresh = stale; review = needs judgement') . ')</small></dd>'
      . '<dt>' . t('Similarity score') . '</dt><dd>' . round($cluster['similarity_score'], 3)
        . ' <small>(' . t('0–1; semantic similarity within cluster') . ')</small></dd>'
      . '<dt>' . t('Age score') . '</dt><dd>' . round($cluster['age_score'], 3)
        . ' <small>(' . t('smooth decay; 0.0 = recent, 1.0 = fully stale') . ')</small></dd>'
      . '<dt>' . t('Traffic score') . '</dt><dd>' . round($cluster['traffic_score'], 3)
        . ' <small>(' . t('normalized GA4 pageviews; requires CSV upload') . ')</small></dd>'
      . '<dt>' . t('Consolidation priority') . '</dt><dd><strong>' . round($cluster['composite_score'], 3) . '</strong>'
        . ' <small>(' . $weights_label . '; higher = more urgent action needed)</small></dd>'
      . '</dl>',
  ];

  $form['canonical_note'] = [
    '#markup' => !empty($cluster['canonical_nid'])
      ? '<p><strong>' . t('Working canonical page') . ':</strong> '
        . t('This is the working page for this exact duplicate set. No content is changed until an administrator approves an action.')
        . '</p>'
      : ((float) $cluster['similarity_score'] < 0.999
        ? '<p><strong>' . t('Historical semantic result') . ':</strong> '
          . t('This older scan grouped pages by similar wording. It is retained for history only and is not an exact-duplicate finding.')
          . '</p>'
        : '<p><strong>' . t('Exact duplicate set') . ':</strong> '
          . t('These pages have matching normalized titles and editable content. Review the working page and candidates before approving an action.')
          . '</p>'),
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
  $node_age = [];
  $live_titles = [];
  if (!empty($all_nids)) {
    $node_rows = db_query(
      'SELECT nid, title, changed FROM {node} WHERE nid IN (:nids)',
      [':nids' => $all_nids]
    )->fetchAllAssoc('nid', PDO::FETCH_ASSOC);
    foreach ($node_rows as $nid => $row) {
      $live_titles[(int) $nid] = $row['title'];
      $node_age[(int) $nid] = ai_content_audit_node_age_score((int) $row['changed']);
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

  $max_node_traffic = !empty($node_traffic) ? max($node_traffic) : 0;

  $options = [];
  foreach ($candidates as $c) {
    $nid = (int) $c['nid'];
    $display_title = isset($live_titles[$nid]) ? $live_titles[$nid] : $c['title'];
    $node_link = l(check_plain($display_title), 'node/' . $nid, ['attributes' => ['target' => '_blank']]);
    $is_canonical = !empty($c['is_canonical']);
    $nid_label = '<span style="color:var(--gin-color-text-light,#999);font-size:0.85em;margin-left:5px">#' . $nid . '</span>';
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

    $cand_age = isset($node_age[$nid]) ? (float) $node_age[$nid] : 0.0;
    $node_traffic_norm = ($max_node_traffic > 0 && isset($node_traffic[$nid])) ? ($node_traffic[$nid] / $max_node_traffic) : 0.0;
    $cand_priority = round(
      ((float) $c['similarity_score'] * $weights['similarity']) +
      ($cand_age * $weights['age']) +
      ($node_traffic_norm * $weights['traffic']),
      3
    );

    $options[$c['acid']] = [
      $title_cell,
      check_plain($c['bundle']),
      check_plain($c['task_type']),
      check_plain($c['status']),
      round($c['similarity_score'], 3),
      isset($node_age[$nid]) ? round($node_age[$nid], 3) : '—',
      isset($node_traffic[$nid]) ? number_format($node_traffic[$nid]) : '—',
      $cand_priority,
      ['data' => $rot_badge],
      check_plain($cluster['recommended_action']),
      ['data' => $lifecycle],
    ];
  }

  $form['candidates'] = [
    '#type' => 'tableselect',
    '#header' => [t('Title'), t('Type'), t('Task'), t('Status'), t('Similarity'), t('Age'), t('Traffic'), t('Priority'), t('ROT'), t('Action'), t('Lifecycle')],
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
    '#access' => !empty($cluster['canonical_nid']) && in_array($cluster['recommended_action'], ['merge', 'refresh'], TRUE),
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
    if (!$cluster || !in_array($cluster['recommended_action'], ['merge', 'refresh'], TRUE)) {
      backdrop_set_message(t('AI edits are limited to clusters with a merge or refresh recommendation. This cluster is still for human review.'), 'warning');
      $form_state['redirect'] = 'admin/config/ai/content-audit/clusters/' . $cid;
      return;
    }
    if ($cluster && ai_content_audit_auto_draft_cluster_edit($cluster)) {
      backdrop_set_message(t('AI draft revision proposed for the canonical page. Review the diff and approve it from the queue or this cluster view.'));
    }
    else {
      backdrop_set_message(t('No AI draft revision was created for this cluster. The canonical page may lack editable body content, already have a pending draft, or the model may have declined to propose changes.'), 'warning');
    }
    $form_state['redirect'] = 'admin/config/ai/content-audit/clusters/' . $cid;
    return;
  }

  if ($op === 'set_canonical') {
    $selected = [];
    if (!empty($form_state['values']['candidates']) && is_array($form_state['values']['candidates'])) {
      foreach ($form_state['values']['candidates'] as $acid => $checked) {
        if ($checked) {
          $selected[] = (int) $acid;
        }
      }
    }
    if (count($selected) !== 1) {
      backdrop_set_message(t('Select exactly one candidate to set as canonical.'), 'warning');
    }
    elseif (ai_content_audit_set_canonical($cid, $selected[0])) {
      backdrop_set_message(t('Canonical page updated.'));
    }
    else {
      backdrop_set_message(t('The canonical page could not be changed. Reject or apply any existing AI draft before switching it.'), 'warning');
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
       (ca.status = :pending_status AND ca.task_type NOT IN (:scan_types))
       OR ca.status IN (:statuses)
     )
     ORDER BY ca.updated DESC LIMIT 200',
    [
      ':pending_status' => 'pending',
    ':scan_types'     => ['canonical', 'duplicate_candidate', 'related'],
      ':statuses'       => ['approved', 'applied', 'rejected', 'failed', 'edit_queued', 'ignored'],
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
      check_plain($c['topic_label'] ?: t('Individual ROT assessment')),
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
  $form['actions']['approve'] = [
    '#type' => 'submit',
    '#value' => t('Approve selected'),
    '#op' => 'approve',
  ];
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

  if ($op === 'approve' && $selected) {
    $approvable = db_query(
      'SELECT acid FROM {ai_content_audit_candidate}
       WHERE acid IN (:acids) AND status = :status
         AND task_type NOT IN (:scan_types)',
      [
        ':acids' => $selected,
        ':status' => 'pending',
        ':scan_types' => ['canonical', 'duplicate_candidate', 'related'],
      ]
    )->fetchCol();
    if ($approvable) {
      ai_content_audit_update_candidate_status($approvable, 'approved', $GLOBALS['user']->uid);
      backdrop_set_message(t('Approved @count action(s). Apply them when ready.', ['@count' => count($approvable)]));
    }
    else {
      backdrop_set_message(t('The selected items are not pending approval.'), 'warning');
    }
  }
  elseif ($op === 'approve') {
    backdrop_set_message(t('Select at least one pending action to approve.'), 'warning');
  }
  elseif ($op === 'apply') {
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

  $form['scan'] = [
    '#type' => 'fieldset',
    '#title' => t('Scanner settings'),
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
    '#description' => t('How many nodes to process per cron run. Lower = safer on shared hosting; higher = faster scans.'),
    '#options' => [10 => 10, 25 => 25, 50 => 50, 100 => 100, 200 => 200],
    '#default_value' => $config->get('cron_batch_size') ?: 50,
  ];
  $form['scan']['default_profile'] = [
    '#type' => 'textfield',
    '#title' => t('Default profile'),
    '#default_value' => $config->get('default_profile') ?: 'default',
    '#size' => 32,
  ];

  $form['matching'] = [
    '#type' => 'fieldset',
    '#title' => t('Duplicate matching'),
  ];
  $form['matching']['description'] = [
    '#markup' => '<p>' . t('Scans create clusters only when normalized titles and full editable content match exactly. Similar topics, similar titles, and shared template language are intentionally excluded.') . '</p>',
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

  $form['near_duplicates'] = [
    '#type' => 'fieldset',
    '#title' => t('Near-duplicate confirmation'),
    '#description' => t('Semantic similarity alone keeps near-duplicate pages at "review". Optionally ask a decision model whether each strongly similar pair is actually interchangeable; confirmed pairs recommend merging the weaker page into the stronger one. Merges remain approval-gated.'),
  ];
  $decision_models = function_exists('ai_get_all_decision_models') ? (ai_get_all_decision_models() ?: []) : [];
  $form['near_duplicates']['near_duplicate_decision_model'] = [
    '#type' => 'select',
    '#title' => t('Decision model'),
    '#options' => ['' => t('- None (keep near-duplicates at review) -')] + $decision_models,
    '#default_value' => (string) $config->get('near_duplicate_decision_model'),
  ];
  $nd_threshold = $config->get('near_duplicate_decision_threshold');
  $form['near_duplicates']['near_duplicate_decision_threshold'] = [
    '#type' => 'textfield',
    '#title' => t('Confirmation threshold'),
    '#description' => t('Probability (0-1) that the pages are interchangeable before a merge is recommended. Default is 0.8.'),
    '#default_value' => is_numeric($nd_threshold) ? $nd_threshold : 0.8,
    '#size' => 10,
    '#states' => [
      'invisible' => [
        ':input[name="near_duplicate_decision_model"]' => ['value' => ''],
      ],
    ],
  ];
  $form['near_duplicates']['near_duplicate_decision_limit'] = [
    '#type' => 'select',
    '#title' => t('Pairs checked per scan'),
    '#description' => t('The most similar pairs are checked first.'),
    '#options' => [10 => 10, 25 => 25, 50 => 50, 100 => 100],
    '#default_value' => $config->get('near_duplicate_decision_limit') ?: 25,
    '#states' => [
      'invisible' => [
        ':input[name="near_duplicate_decision_model"]' => ['value' => ''],
      ],
    ],
  ];

  $form['scoring'] = [
    '#type' => 'fieldset',
    '#title' => t('Scoring &amp; Consolidation Priority'),
    '#description' => t('Configure how urgency is weighted and how age decay is calculated across clusters.'),
  ];
  $form['scoring']['weight_similarity'] = [
    '#type' => 'number',
    '#title' => t('Similarity weight (%)'),
    '#description' => t('Weight assigned to semantic and content similarity between pages. Default is 50%.'),
    '#default_value' => $config->get('weight_similarity') ?? 50,
    '#min' => 0,
    '#max' => 100,
    '#step' => 1,
  ];
  $form['scoring']['weight_age'] = [
    '#type' => 'number',
    '#title' => t('Age / staleness weight (%)'),
    '#description' => t('Weight assigned to lack of recent updates. Default is 30%.'),
    '#default_value' => $config->get('weight_age') ?? 30,
    '#min' => 0,
    '#max' => 100,
    '#step' => 1,
  ];
  $form['scoring']['weight_traffic'] = [
    '#type' => 'number',
    '#title' => t('Traffic weight (%)'),
    '#description' => t('Weight assigned to normalized GA4 pageviews. Default is 20%. If no GA4 data has been uploaded, similarity and age weights automatically re-normalize so maximum priority remains 1.0.'),
    '#default_value' => $config->get('weight_traffic') ?? 20,
    '#min' => 0,
    '#max' => 100,
    '#step' => 1,
  ];
  $form['scoring']['decay_start_months'] = [
    '#type' => 'number',
    '#title' => t('Decay start threshold (months)'),
    '#description' => t('Content younger than this has zero age decay (0.0). Default is 6 months.'),
    '#default_value' => $config->get('decay_start_months') ?? 6,
    '#min' => 0,
    '#max' => 60,
    '#step' => 1,
  ];
  $form['scoring']['stale_age_months'] = [
    '#type' => 'number',
    '#title' => t('Full staleness threshold (months)'),
    '#description' => t('Content older than this reaches maximum age decay (1.0). Default is 24 months (2 years).'),
    '#default_value' => $config->get('stale_age_months') ?? 24,
    '#min' => 1,
    '#max' => 120,
    '#step' => 1,
  ];

  $form['#attributes']['enctype'] = 'multipart/form-data';
  $form = system_settings_form($form);
  $form['#validate'][] = 'ai_content_audit_settings_form_validate';
  $form['#submit'] = ['ai_content_audit_settings_form_submit'];
  return $form;
}

/**
 * Settings form validate handler.
 */
function ai_content_audit_settings_form_validate($form, &$form_state) {
  $w_sim = isset($form_state['values']['weight_similarity']) ? (float) $form_state['values']['weight_similarity'] : 0;
  $w_age = isset($form_state['values']['weight_age']) ? (float) $form_state['values']['weight_age'] : 0;
  $w_traf = isset($form_state['values']['weight_traffic']) ? (float) $form_state['values']['weight_traffic'] : 0;

  if ($w_sim < 0 || $w_age < 0 || $w_traf < 0) {
    form_set_error('weight_similarity', t('Priority weights cannot be negative numbers.'));
  }
  if (($w_sim + $w_age + $w_traf) <= 0) {
    form_set_error('weight_similarity', t('The sum of priority weights must be greater than 0.'));
  }

  $decay_start = isset($form_state['values']['decay_start_months']) ? (int) $form_state['values']['decay_start_months'] : 6;
  $stale_age = isset($form_state['values']['stale_age_months']) ? (int) $form_state['values']['stale_age_months'] : 24;
  if ($decay_start >= $stale_age) {
    form_set_error('decay_start_months', t('Decay start threshold must be strictly less than the full staleness threshold.'));
  }

  $nd_threshold = $form_state['values']['near_duplicate_decision_threshold'] ?? 0.8;
  if (!is_numeric($nd_threshold) || $nd_threshold < 0 || $nd_threshold > 1) {
    form_set_error('near_duplicate_decision_threshold', t('The confirmation threshold must be a number between 0 and 1.'));
  }
  else {
    form_set_value($form['near_duplicates']['near_duplicate_decision_threshold'], (float) $nd_threshold, $form_state);
  }
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

  while (($row = fgetcsv($handle, 0, ',', '"', '\\')) !== FALSE) {
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
    '#markup' => '<p style="color:var(--gin-color-text-light,#666);font-size:0.9em">' . implode(' · ', $diag_lines) . '</p>',
  ];

  // Show the raw extracted source on both sides so you can see what was fed
  // into the diff (useful when the diff comes up empty).
  $out['raw'] = [
    '#markup' => '<details><summary>' . t('Raw extracted text (debug)') . '</summary>'
      . '<h4>' . t('Original (raw value)') . '</h4>'
      . '<pre style="white-space:pre-wrap;background:var(--gin-bg-layer2,#f5f5f5);color:var(--gin-color-text,inherit);padding:.5em;max-height:300px;overflow:auto">' . check_plain($original_body) . '</pre>'
      . '<h4>' . t('Proposed (raw value)') . '</h4>'
      . '<pre style="white-space:pre-wrap;background:var(--gin-bg-layer2,#f5f5f5);color:var(--gin-color-text,inherit);padding:.5em;max-height:300px;overflow:auto">' . check_plain($proposed_body) . '</pre>'
      . '</details>',
  ];

  // Title diff.
  if ($original_node->title !== $proposed_node->title) {
    $out['title'] = [
      '#markup' => '<h3>' . t('Title') . '</h3>'
        . '<div style="padding:.5em;background:var(--gin-status-danger-bg,#fee);color:var(--gin-color-text,inherit);text-decoration:line-through;margin-bottom:.25em">' . check_plain($original_node->title) . '</div>'
        . '<div style="padding:.5em;background:var(--gin-status-success-bg,#efe);color:var(--gin-color-text,inherit)">' . check_plain($proposed_node->title) . '</div>',
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
    $out['body_heading'] = ['#markup' => '<h3>' . t('Body') . ' <span style="font-weight:normal;color:var(--gin-color-text-light,#666);font-size:.8em">' . t('(inline word diff, markup stripped)') . '</span></h3>'];
    if (module_exists('diff')) {
      module_load_include('inc', 'diff', 'includes/diff.backdrop_diff_inline');
      $renderer = new BackdropDiffInline($original_text, $proposed_text);
      $out['body_diff'] = [
        '#markup' => '<div class="diff-inline" style="padding:1em;border:1px solid var(--gin-border-color,#ccc);background:var(--gin-bg-layer2,#fafafa);color:var(--gin-color-text,inherit);white-space:pre-wrap;font-family:inherit;line-height:1.5">' . $renderer->render() . '</div>',
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
          . '.diff-inline .diff-added,.diff-inline ins{background:var(--gin-status-success-bg,#dfd);text-decoration:none;padding:0 2px}'
          . '.diff-inline .diff-deleted,.diff-inline del{background:var(--gin-status-danger-bg,#fdd);text-decoration:line-through;padding:0 2px}'
          . '.diff-inline .diff-changed{background:var(--gin-status-warning-bg,#ffd);padding:0 2px}'
          . '</style>',
      ];
    }
    else {
      $out['body_diff'] = [
        '#markup' => '<h4>' . t('Original') . '</h4><pre style="white-space:pre-wrap;background:var(--gin-status-danger-bg,#fee);color:var(--gin-color-text,inherit);padding:.5em">' . check_plain($original_text) . '</pre>'
          . '<h4>' . t('Proposed') . '</h4><pre style="white-space:pre-wrap;background:var(--gin-status-success-bg,#efe);color:var(--gin-color-text,inherit);padding:.5em">' . check_plain($proposed_text) . '</pre>',
      ];
    }
  }

  return $out;
}
