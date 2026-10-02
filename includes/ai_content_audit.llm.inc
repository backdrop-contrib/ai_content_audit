<?php

/**
 * @file
 * LLM-driven cluster summaries and draft edit proposals for AI Content Audit.
 *
 * Extracted from ai_content_audit.scanner.inc. Loaded on demand by the
 * scanner finalize step, by the admin "propose AI edit" form action, and by
 * the ai_async summarize callback. Scanner orchestration, clustering, and
 * scoring stay in scanner.inc — anything that talks to an LLM lives here.
 *
 * Field-level helpers used by the auto-draft pipeline live in
 * ai_content_audit.fields.inc and are loaded at the top of this file.
 */

module_load_include('inc', 'ai_content_audit', 'includes/ai_content_audit.fields');

// ---------------------------------------------------------------------------
// Cluster summaries
// ---------------------------------------------------------------------------

/**
 * Submit one ai_async job per cluster needing a summary.
 */
function _ai_content_audit_submit_summary_jobs($run_id) {
  $clusters = db_query(
    'SELECT cid FROM {ai_content_audit_cluster} WHERE run_id = :rid AND llm_summary IS NULL',
    [':rid' => $run_id]
  )->fetchCol();

  foreach ($clusters as $cid) {
    ai_async_submit('ai_content_audit_async_summarize', ['cid' => (int) $cid]);
  }
}

/**
 * ai_async callback: generate LLM summary for one cluster.
 */
function ai_content_audit_async_summarize(array $data) {
  $cid = !empty($data['cid']) ? (int) $data['cid'] : 0;
  if (!$cid) {
    return ['error' => 'No cid'];
  }

  $cluster = db_query(
    'SELECT c.cid, c.topic_label, c.recommended_action, c.source_count, c.similarity_score,
            GROUP_CONCAT(ca.title ORDER BY ca.is_canonical DESC SEPARATOR \' | \') AS titles
     FROM {ai_content_audit_cluster} c
     LEFT JOIN {ai_content_audit_candidate} ca ON ca.cluster_id = c.cid
     WHERE c.cid = :cid
     GROUP BY c.cid',
    [':cid' => $cid]
  )->fetchObject();

  if (!$cluster) {
    return ['error' => 'Cluster not found'];
  }

  $prompt = "Cluster topic: {$cluster->topic_label}\n"
    . "Pages in cluster: {$cluster->source_count}\n"
    . "Titles: {$cluster->titles}\n"
    . "Similarity score: {$cluster->similarity_score}\n"
    . "Recommended action: {$cluster->recommended_action}\n\n"
    . "Write 2 sentences explaining why these pages overlap and what the recommended action means for an editor. Be specific about the content.";

  $summary = ai_content_audit_llm_summarize($prompt);
  if ($summary) {
    db_update('ai_content_audit_cluster')
      ->fields(['llm_summary' => $summary, 'updated' => REQUEST_TIME])
      ->condition('cid', $cid)
      ->execute();
  }

  return ['cid' => $cid, 'ok' => !empty($summary)];
}

/**
 * Use LLM to generate short summaries for pending clusters in a run (inline).
 */
function ai_content_audit_summarize_clusters($run_id) {
  $clusters = db_query(
    'SELECT c.cid, c.topic_label, c.recommended_action, c.source_count, c.similarity_score,
            GROUP_CONCAT(ca.title ORDER BY ca.is_canonical DESC SEPARATOR \' | \') AS titles
     FROM {ai_content_audit_cluster} c
     LEFT JOIN {ai_content_audit_candidate} ca ON ca.cluster_id = c.cid
     WHERE c.run_id = :rid AND c.llm_summary IS NULL
     GROUP BY c.cid
     LIMIT 20',
    [':rid' => $run_id]
  );

  foreach ($clusters as $cluster) {
    $prompt = "Cluster topic: {$cluster->topic_label}\n"
      . "Pages in cluster: {$cluster->source_count}\n"
      . "Titles: {$cluster->titles}\n"
      . "Similarity score: {$cluster->similarity_score}\n"
      . "Recommended action: {$cluster->recommended_action}\n\n"
      . "Write 2 sentences explaining why these pages overlap and what the recommended action means for an editor. Be specific about the content.";

    $summary = ai_content_audit_llm_summarize($prompt);
    if ($summary) {
      db_update('ai_content_audit_cluster')
        ->fields(['llm_summary' => $summary, 'updated' => REQUEST_TIME])
        ->condition('cid', $cluster->cid)
        ->execute();
    }
  }
}

/**
 * Call the AI chat API for a single short completion.
 */
function ai_content_audit_llm_summarize($prompt) {
  if (!function_exists('ai_chat')) {
    return '';
  }
  $config = config('ai_content_audit.settings');
  $model = $config->get('summary_model') ?: '';
  if ($model === '') {
    return '';
  }
  try {
    $messages = [
      ['role' => 'system', 'content' => 'You are a content strategist. Write concise, specific summaries for content editors.'],
      ['role' => 'user', 'content' => $prompt],
    ];
    $response = ai_chat($model, $messages, 0.3, 200, FALSE, ['operation' => 'content_audit_summary']);
    if (is_string($response)) {
      return trim($response);
    }
    return isset($response->content) ? trim($response->content) : '';
  }
  catch (\Exception $e) {
    watchdog('ai_content_audit', 'LLM summary error: @msg', ['@msg' => $e->getMessage()], WATCHDOG_WARNING);
    return '';
  }
}

// ---------------------------------------------------------------------------
// Auto-drafted canonical edits
// ---------------------------------------------------------------------------

/**
 * Generate draft edit proposals for high-value clusters after a scan.
 */
function ai_content_audit_auto_draft_cluster_edits($run_id) {
  module_load_include('inc', 'ai_content_audit', 'includes/ai_content_audit.tools');

  $config = config('ai_content_audit.settings');
  $limit = max(1, min(20, (int) ($config->get('auto_draft_edit_limit') ?: 5)));

  $query = db_select('ai_content_audit_cluster', 'c');
  $query->fields('c', ['cid', 'topic_label', 'recommended_action', 'llm_summary']);
  $query->condition('run_id', $run_id);
  $query->condition('review_state', 'pending');
  $query->condition('recommended_action', ['merge', 'refresh'], 'IN');
  $query->orderBy('composite_score', 'DESC');
  $query->orderBy('cid', 'ASC');
  $query->range(0, $limit);
  $clusters = $query->execute()->fetchAllAssoc('cid', PDO::FETCH_ASSOC);

  $drafted = 0;
  foreach ($clusters as $cluster) {
    if (ai_content_audit_auto_draft_cluster_edit($cluster)) {
      $drafted++;
    }
  }

  if ($drafted) {
    watchdog('ai_content_audit', 'Created @count AI draft edit proposal(s) for run @run_id.', [
      '@count' => $drafted,
      '@run_id' => $run_id,
    ]);
  }
}

/**
 * Generate one draft edit proposal for a cluster's canonical page.
 */
function ai_content_audit_auto_draft_cluster_edit(array $cluster) {
  $candidates = ai_content_audit_load_candidates((int) $cluster['cid']);
  if (empty($candidates)) {
    return FALSE;
  }

  $canonical = NULL;
  $duplicates = [];
  foreach ($candidates as $candidate) {
    if (!empty($candidate['is_canonical'])) {
      $canonical = $candidate;
    }
    else {
      $duplicates[] = $candidate;
    }
  }

  if (!$canonical || empty($duplicates)) {
    return FALSE;
  }

  if ($canonical['status'] !== 'pending' || $canonical['task_type'] !== 'canonical') {
    return FALSE;
  }

  foreach ($candidates as $candidate) {
    if ($candidate['task_type'] === 'edit_proposed' && in_array($candidate['status'], ['pending', 'approved', 'applied'], TRUE)) {
      return FALSE;
    }
  }

  $canonical_content = ai_content_audit_execute_get_node_content(['nid' => (int) $canonical['nid']]);
  if (!empty($canonical_content['error']) || empty($canonical_content['rendered_content']) || empty($canonical_content['primary_edit_target'])) {
    return FALSE;
  }
  $target_context = $canonical_content['primary_edit_target'];
  $target_field = !empty($target_context['field_name']) ? $target_context['field_name'] : '';
  $current_body = !empty($target_context['value']) ? $target_context['value'] : '';
  $rendered_content = $canonical_content['rendered_content'];
  $normalized_field = ai_content_audit_normalize_text_for_compare($current_body);
  $normalized_rendered = ai_content_audit_normalize_text_for_compare($rendered_content);
  if ($normalized_field === '' || $normalized_rendered === '') {
    return FALSE;
  }
  if (strpos($normalized_rendered, $normalized_field) === FALSE && strpos($normalized_field, $normalized_rendered) === FALSE) {
    watchdog('ai_content_audit', 'Skipping AI draft for cluster @cid: writable field @field does not match the page main content.', [
      '@cid' => $cluster['cid'],
      '@field' => $target_field,
    ], WATCHDOG_NOTICE);
    return FALSE;
  }

  $supporting_nodes = [];
  foreach (array_slice($duplicates, 0, 3) as $candidate) {
    $node_content = ai_content_audit_execute_get_node_content(['nid' => (int) $candidate['nid']]);
    if (!empty($node_content['error'])) {
      continue;
    }
    $supporting_nodes[] = $node_content;
  }

  if (empty($supporting_nodes)) {
    return FALSE;
  }

  $prompt = ai_content_audit_build_edit_prompt($cluster, $canonical, $canonical_content, $supporting_nodes);
  $proposal = ai_content_audit_llm_generate_edit($prompt);
  if (empty($proposal) || empty($proposal['should_draft'])) {
    return FALSE;
  }

  $proposed_title = isset($proposal['proposed_title']) ? trim($proposal['proposed_title']) : '';
  $proposed_body = isset($proposal['proposed_body']) ? trim($proposal['proposed_body']) : '';
  $proposed_summary = isset($proposal['proposed_summary']) ? trim($proposal['proposed_summary']) : '';
  $reason = isset($proposal['reason']) ? trim($proposal['reason']) : '';

  $current_title = trim($canonical_content['title']);
  $current_body = trim($current_body);
  $current_summary = !empty($target_context['summary']) ? trim($target_context['summary']) : '';

  if ($proposed_body === '') {
    return FALSE;
  }
  if ($proposed_title === '') {
    $proposed_title = $current_title;
  }
  if (
    ai_content_audit_normalize_text_for_compare($proposed_title) === ai_content_audit_normalize_text_for_compare($current_title) &&
    ai_content_audit_normalize_text_for_compare($proposed_body) === ai_content_audit_normalize_text_for_compare($current_body) &&
    ai_content_audit_normalize_text_for_compare($proposed_summary) === ai_content_audit_normalize_text_for_compare($current_summary)
  ) {
    return FALSE;
  }
  if ($reason === '') {
    $reason = t('Auto-drafted canonical update from clustered related pages.');
  }

  $result = ai_content_audit_execute_propose_edit([
    'acid' => (int) $canonical['acid'],
    'nid' => (int) $canonical['nid'],
    'target_context' => $target_context,
    'target_field' => $target_field,
    'proposed_title' => $proposed_title,
    'proposed_body' => $proposed_body,
    'proposed_summary' => $proposed_summary,
    'reason' => $reason,
  ]);

  if (!empty($result['error'])) {
    watchdog('ai_content_audit', 'Auto-draft edit failed for cluster @cid / node @nid: @msg', [
      '@cid' => $cluster['cid'],
      '@nid' => $canonical['nid'],
      '@msg' => $result['error'],
    ], WATCHDOG_WARNING);
    return FALSE;
  }

  return TRUE;
}

/**
 * Generate one draft edit proposal for an individual ROT assessment.
 *
 * This is intentionally separate from cluster canonical-page drafting. A ROT
 * page may be old or trivial without being a duplicate of another page, so it
 * has no canonical/supporting-page relationship to resolve first.
 *
 * @param array $score
 *   A row from ai_content_audit_node_score.
 *
 * @return array
 *   Result data, or an array containing an error string.
 */
function ai_content_audit_auto_draft_node_edit(array $score) {
  module_load_include('inc', 'ai_content_audit', 'includes/ai_content_audit.tools');

  $existing = db_query(
    "SELECT * FROM {ai_content_audit_candidate}
      WHERE run_id = :run_id AND nid = :nid
        AND status NOT IN (:statuses)
      ORDER BY acid DESC LIMIT 1",
    [
      ':run_id' => $score['run_id'],
      ':nid' => (int) $score['nid'],
      ':statuses' => ['rejected', 'ignored', 'applied', 'failed'],
    ]
  )->fetchAssoc();

  if ($existing && $existing['task_type'] === 'edit_proposed' && in_array($existing['status'], ['pending', 'approved'], TRUE)) {
    return ['error' => 'This page already has a pending AI draft revision.'];
  }

  $acid = $existing ? (int) $existing['acid'] : 0;

  $node_content = ai_content_audit_execute_get_node_content(['nid' => (int) $score['nid']]);
  if (!empty($node_content['error']) || empty($node_content['primary_edit_target']['value'])) {
    return ['error' => 'This page does not have editable text content for an AI draft.'];
  }

  $target_context = $node_content['primary_edit_target'];
  $target_field = !empty($node_content['primary_edit_field'])
    ? $node_content['primary_edit_field']
    : (!empty($target_context['field_name']) ? $target_context['field_name'] : '');
  if ($target_field === '') {
    return ['error' => 'This page does not have a recognized editable text field.'];
  }

  $prompt = ai_content_audit_build_node_edit_prompt($score, $node_content);
  $proposal = ai_content_audit_llm_generate_edit($prompt);
  if (empty($proposal) || empty($proposal['should_draft'])) {
    return ['error' => 'The configured model declined to propose a substantive edit.'];
  }

  $proposed_title = isset($proposal['proposed_title']) ? trim($proposal['proposed_title']) : '';
  $proposed_body = isset($proposal['proposed_body']) ? trim($proposal['proposed_body']) : '';
  $proposed_summary = isset($proposal['proposed_summary']) ? trim($proposal['proposed_summary']) : '';
  $reason = isset($proposal['reason']) ? trim($proposal['reason']) : '';
  $current_title = trim($node_content['title']);
  $current_body = trim($target_context['value']);
  $current_summary = !empty($target_context['summary']) ? trim($target_context['summary']) : '';

  if ($proposed_body === '') {
    return ['error' => 'The model did not return editable body content.'];
  }
  if ($proposed_title === '') {
    $proposed_title = $current_title;
  }
  if (
    ai_content_audit_normalize_text_for_compare($proposed_title) === ai_content_audit_normalize_text_for_compare($current_title) &&
    ai_content_audit_normalize_text_for_compare($proposed_body) === ai_content_audit_normalize_text_for_compare($current_body) &&
    ai_content_audit_normalize_text_for_compare($proposed_summary) === ai_content_audit_normalize_text_for_compare($current_summary)
  ) {
    return ['error' => 'The model did not propose a meaningful content change.'];
  }
  if ($reason === '') {
    $reason = t('AI-suggested refresh based on the page ROT assessment.');
  }

  if (!$acid) {
    $acid = ai_content_audit_queue_rot_candidate($score);
    if (!$acid) {
      return ['error' => 'Could not create an editorial queue item for this page.'];
    }
  }

  $result = ai_content_audit_execute_propose_edit([
    'acid' => $acid,
    'nid' => (int) $score['nid'],
    'target_context' => $target_context,
    'target_field' => $target_field,
    'proposed_title' => $proposed_title,
    'proposed_body' => $proposed_body,
    'proposed_summary' => $proposed_summary,
    'reason' => $reason,
  ]);

  if (!empty($result['error'])) {
    return ['error' => $result['error']];
  }

  return $result;
}

/**
 * Build a prompt for drafting an individual ROT-page edit.
 */
function ai_content_audit_build_node_edit_prompt(array $score, array $node_content) {
  $target_field = !empty($node_content['primary_edit_field']) ? $node_content['primary_edit_field'] : 'body';
  $source = ai_content_audit_extract_edit_source_text($node_content);
  $lifecycle = !empty($node_content['lifecycle']['ai_analysis'])
    ? trim($node_content['lifecycle']['ai_analysis'])
    : 'None';
  $summary = !empty($node_content['primary_edit_target']['summary'])
    ? trim($node_content['primary_edit_target']['summary'])
    : 'None';

  return "You are suggesting a careful editorial refresh for one existing web page.\n"
    . "Return JSON only with this exact shape:\n"
    . "{\"should_draft\":true|false,\"proposed_title\":\"...\",\"proposed_body\":\"...\",\"proposed_summary\":\"...\",\"reason\":\"...\"}\n\n"
    . "Rules:\n"
    . "- Preserve the page's intent and URL identity.\n"
    . "- Do not invent facts, dates, names, statistics, or claims.\n"
    . "- Use only information already present in the source below.\n"
    . "- If the page cannot be improved confidently, return should_draft false.\n"
    . "- proposed_body must be the raw value for the editable field only. Do not add admin links, field labels, paragraph wrappers, or markup not present in the source.\n"
    . "- If the source has no HTML, return plain text with blank lines between paragraphs.\n"
    . "- Do not make cosmetic-only changes such as whitespace, capitalization, punctuation, or markup.\n"
    . "- The reason must explain the substantive change in 1-2 sentences.\n\n"
    . "Page title: {$node_content['title']}\n"
    . "ROT classification: {$score['rot_type']}\n"
    . "Recommended action: {$score['recommended_action']}\n"
    . "Age score: {$score['age_score']}\n"
    . "Traffic score: {$score['traffic_score']}\n"
    . "Priority score: {$score['composite_score']}\n"
    . "Primary editable field: {$target_field}\n"
    . "Current summary: {$summary}\n"
    . "Lifecycle analysis: {$lifecycle}\n"
    . "Editable field source (write proposed_body in this same form):\n"
    . ai_content_audit_trim_text($source, 6000);
}

/**
 * Build a prompt for drafting a canonical-page edit.
 */
function ai_content_audit_build_edit_prompt(array $cluster, array $canonical, array $canonical_content, array $supporting_nodes) {
  $canonical_lifecycle = !empty($canonical_content['lifecycle']['ai_analysis']) ? trim($canonical_content['lifecycle']['ai_analysis']) : 'None';
  $canonical_summary = !empty($canonical_content['fields']['body_summary']) ? trim($canonical_content['fields']['body_summary']) : '';
  $target_field = !empty($canonical_content['primary_edit_field']) ? $canonical_content['primary_edit_field'] : 'body';

  // Use the raw editable field source — never the rendered HTML — so the model
  // does not echo paragraph wrappers, admin dropbuttons, or other view markup
  // back into the body.
  $canonical_source = '';
  if (!empty($canonical_content['primary_edit_target']['value'])) {
    $canonical_source = $canonical_content['primary_edit_target']['value'];
  }
  elseif (!empty($canonical_content['fields']['body'])) {
    $canonical_source = $canonical_content['fields']['body'];
  }

  $supporting_blocks = [];
  foreach ($supporting_nodes as $node_content) {
    $excerpt = ai_content_audit_extract_edit_source_text($node_content);
    $supporting_blocks[] = "Title: {$node_content['title']}\n"
      . "URL: {$node_content['edit_url']}\n"
      . "Lifecycle analysis: " . (!empty($node_content['lifecycle']['ai_analysis']) ? trim($node_content['lifecycle']['ai_analysis']) : 'None') . "\n"
      . "Source text excerpt:\n{$excerpt}";
  }

  $prompt = "You are editing the canonical page for a content-audit cluster.\n"
    . "Return JSON only with this exact shape:\n"
    . "{\"should_draft\":true|false,\"proposed_title\":\"...\",\"proposed_body\":\"...\",\"proposed_summary\":\"...\",\"reason\":\"...\"}\n\n"
    . "Rules:\n"
    . "- Only draft edits for the canonical page.\n"
    . "- Do not invent facts, dates, names, or claims.\n"
    . "- Preserve the existing page intent and URL identity.\n"
    . "- Incorporate only clearly useful, non-duplicative details from related pages.\n"
    . "- If there is not enough trustworthy information to improve the canonical page, return should_draft false.\n"
    . "- proposed_body must be the raw value for the editable field only. Do NOT include <div>, paragraph wrappers, admin links, dropbuttons, field labels, or any markup you did not see in the field source below. If the source has no HTML, return plain text with blank lines between paragraphs.\n"
    . "- If the only changes you would make are cosmetic (whitespace, capitalization, punctuation, or markup), return should_draft false.\n"
    . "- The reason must explain what changed and why in 1-2 sentences.\n\n"
    . "Cluster topic: {$cluster['topic_label']}\n"
    . "Cluster recommended action: {$cluster['recommended_action']}\n"
    . "Cluster summary: " . (!empty($cluster['llm_summary']) ? trim($cluster['llm_summary']) : 'None') . "\n\n"
    . "Canonical page title: {$canonical_content['title']}\n"
    . "Primary editable field: {$target_field}\n"
    . "Canonical lifecycle analysis: {$canonical_lifecycle}\n"
    . "Canonical body summary: " . ($canonical_summary !== '' ? $canonical_summary : 'None') . "\n"
    . "Canonical editable field source (write your proposed_body in this same form):\n"
    . ai_content_audit_trim_text($canonical_source, 6000) . "\n\n"
    . "Related pages to mine for unique useful details:\n\n"
    . implode("\n\n---\n\n", $supporting_blocks);

  return $prompt;
}

/**
 * Ask the configured edit model to return a JSON edit proposal.
 */
function ai_content_audit_llm_generate_edit($prompt) {
  if (!function_exists('ai_chat')) {
    return [];
  }

  $config = config('ai_content_audit.settings');
  $model = $config->get('auto_draft_edit_model') ?: '';
  if ($model === '') {
    return [];
  }

  try {
    $messages = [
      [
        'role' => 'system',
        'content' => 'You are a careful web content editor. Respond with JSON only and never include markdown fences.',
      ],
      ['role' => 'user', 'content' => $prompt],
    ];
    $response = ai_chat($model, $messages, 0.2, 1600, FALSE, ['operation' => 'content_audit_edit_draft']);
    $raw = is_string($response) ? trim($response) : (isset($response->content) ? trim($response->content) : '');
    return ai_content_audit_decode_json_response($raw);
  }
  catch (\Exception $e) {
    watchdog('ai_content_audit', 'LLM edit draft error: @msg', ['@msg' => $e->getMessage()], WATCHDOG_WARNING);
    return [];
  }
}

/**
 * Decode a JSON object, tolerating fenced responses.
 */
function ai_content_audit_decode_json_response($raw) {
  if ($raw === '') {
    return [];
  }

  $raw = trim($raw);
  if (strpos($raw, '```') === 0) {
    $raw = preg_replace('/^```(?:json)?\s*/', '', $raw);
    $raw = preg_replace('/\s*```$/', '', $raw);
    $raw = trim($raw);
  }

  $decoded = json_decode($raw, TRUE);
  return is_array($decoded) ? $decoded : [];
}

/**
 * Extract the most useful editable text from a node payload.
 */
function ai_content_audit_extract_edit_source_text(array $node_content) {
  // Prefer raw editable-field source over the fully rendered HTML so the model
  // sees text instead of paragraph admin / wrapper markup.
  if (!empty($node_content['primary_edit_target']['value'])) {
    return ai_content_audit_trim_text($node_content['primary_edit_target']['value'], 2500);
  }

  $fields = !empty($node_content['fields']) && is_array($node_content['fields']) ? $node_content['fields'] : [];

  if (!empty($fields['body'])) {
    return ai_content_audit_trim_text($fields['body'], 2500);
  }

  if (!empty($node_content['rendered_content'])) {
    return ai_content_audit_trim_text($node_content['rendered_content'], 2500);
  }

  $chunks = [];
  foreach ($fields as $field_name => $value) {
    if ($field_name === 'body_summary' || $field_name === 'body_format' || !is_string($value) || trim($value) === '') {
      continue;
    }
    $chunks[] = strtoupper($field_name) . ":\n" . ai_content_audit_trim_text($value, 1200);
  }

  return !empty($chunks) ? implode("\n\n", $chunks) : '';
}

// ---------------------------------------------------------------------------
// Near-duplicate confirmation
// ---------------------------------------------------------------------------

/**
 * Ask a decision model whether strongly similar page pairs are interchangeable.
 *
 * Only runs when near_duplicate_decision_model is configured. Pairs come from
 * this run's similarity edges at or above the actionable threshold, most
 * similar first, skipping exact duplicates (they have their own workflow).
 * A failed call leaves that pair at review.
 *
 * @param string $run_id
 *   The audit run ID; its edges must not have been deleted yet.
 * @param array $nodes
 *   Inventory rows keyed by nid.
 * @param array $duplicate_nids
 *   Exact-duplicate nids to skip.
 *
 * @return array
 *   Confirmed merge sources keyed by nid, each mapped to the page it should
 *   merge into (the pair's canonical page).
 */
function ai_content_audit_confirm_near_duplicates($run_id, array $nodes, array $duplicate_nids = []) {
  $config = config('ai_content_audit.settings');
  $model = (string) $config->get('near_duplicate_decision_model');
  if ($model === '' || !module_exists('ai') || !db_table_exists('ai_content_audit_edge')) {
    return [];
  }

  $stripped_model = '';
  $api = ai_get_api_for_model($model, $stripped_model);
  if (!$api || !method_exists($api, 'decide')) {
    watchdog('ai_content_audit', 'Near-duplicate decision model @model is unavailable.', ['@model' => $model], WATCHDOG_WARNING);
    return [];
  }

  $threshold = $config->get('near_duplicate_decision_threshold');
  $threshold = is_numeric($threshold) ? (float) $threshold : 0.8;
  $limit = max(1, (int) ($config->get('near_duplicate_decision_limit') ?: 25));
  $skip = array_fill_keys(array_map('intval', $duplicate_nids), TRUE);

  $edges = db_query(
    'SELECT nid_a, nid_b, MAX(score) AS score FROM {ai_content_audit_edge}
      WHERE run_id = :rid AND score >= :min
      GROUP BY nid_a, nid_b
      ORDER BY score DESC',
    [':rid' => $run_id, ':min' => AI_CONTENT_AUDIT_ACTIONABLE_SIMILARITY_THRESHOLD]
  )->fetchAll(PDO::FETCH_ASSOC);

  $merge_into = [];
  $seen = [];
  $checked = 0;
  foreach ($edges as $edge) {
    if ($checked >= $limit) {
      break;
    }
    $a = (int) $edge['nid_a'];
    $b = (int) $edge['nid_b'];
    $key = min($a, $b) . ':' . max($a, $b);
    if ($a === $b || isset($seen[$key]) || isset($skip[$a]) || isset($skip[$b]) || !isset($nodes[$a], $nodes[$b])) {
      continue;
    }
    $seen[$key] = TRUE;
    // A page already confirmed as a merge source keeps its first (most
    // similar) target.
    if (isset($merge_into[$a]) && isset($merge_into[$b])) {
      continue;
    }
    $checked++;

    $input = "PAGE A\nTitle: " . $nodes[$a]['title'] . "\n" . ai_content_audit_trim_text(ai_content_audit_get_node_audit_text($a), 3000)
      . "\n\nPAGE B\nTitle: " . $nodes[$b]['title'] . "\n" . ai_content_audit_trim_text(ai_content_audit_get_node_audit_text($b), 3000);
    $question = [
      'id' => 'interchangeable',
      'type' => 'boolean',
      // Asymmetric pairs (a long and a short version) count: asking whether
      // nothing would be lost scored those low.
      'prompt' => 'Do page A and page B cover the same specific subject for the same purpose, so that a site editor should merge them into one page instead of keeping both?',
    ];

    try {
      $decisions = $api->decide($input, [$question], $stripped_model ?: $model, ['operation' => 'content_audit_near_duplicate']);
    }
    catch (\Throwable $e) {
      watchdog('ai_content_audit', 'Near-duplicate decision for nodes @a and @b failed: @message', [
        '@a' => $a,
        '@b' => $b,
        '@message' => $e->getMessage(),
      ], WATCHDOG_WARNING);
      continue;
    }

    $decision = NULL;
    foreach ((array) $decisions as $d) {
      if (is_array($d) && ($d['id'] ?? '') === 'interchangeable') {
        $decision = $d;
      }
    }
    if ($decision === NULL || !isset($decision['answer'])) {
      continue;
    }
    if (isset($decision['probabilities']['true']) && is_numeric($decision['probabilities']['true'])) {
      $p_true = (float) $decision['probabilities']['true'];
    }
    elseif (isset($decision['probability']) && is_numeric($decision['probability'])) {
      $p_true = $decision['answer'] ? (float) $decision['probability'] : 1.0 - (float) $decision['probability'];
    }
    else {
      $p_true = $decision['answer'] ? 1.0 : 0.0;
    }
    if ($p_true < $threshold) {
      continue;
    }

    $canonical = ai_content_audit_pick_canonical([$a => $nodes[$a], $b => $nodes[$b]]);
    $source = $canonical === $a ? $b : $a;
    if (!isset($merge_into[$source])) {
      $merge_into[$source] = $canonical;
    }
  }

  return $merge_into;
}

/**
 * Trim text to a max length without dropping all context.
 */
function ai_content_audit_trim_text($text, $max_length = 2000) {
  $text = trim((string) $text);
  if (backdrop_strlen($text) <= $max_length) {
    return $text;
  }
  return rtrim(backdrop_substr($text, 0, $max_length - 3)) . '...';
}
