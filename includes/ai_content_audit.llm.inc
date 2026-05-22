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
