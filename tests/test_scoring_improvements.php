<?php

/**
 * @file
 * Verification test suite for AI Content Audit scoring improvements.
 *
 * Can be executed via CLI:
 *   ddev exec php modules/contrib/ai_content_audit/tests/test_scoring_improvements.php
 */

if (!defined('BACKDROP_ROOT')) {
  define('BACKDROP_ROOT', getcwd());
}

if (!function_exists('backdrop_bootstrap')) {
  require_once BACKDROP_ROOT . '/core/includes/bootstrap.inc';
  backdrop_bootstrap(BACKDROP_BOOTSTRAP_FULL);
}

module_load_include('module', 'ai_content_audit');
module_load_include('inc', 'ai_content_audit', 'includes/ai_content_audit.scoring');
module_load_include('inc', 'ai_content_audit', 'includes/ai_content_audit.scanner');
module_load_include('inc', 'ai_content_audit', 'includes/ai_content_audit.admin');

$passed = 0;
$failed = 0;

function test_assert($condition, $description) {
  global $passed, $failed;
  if ($condition) {
    $passed++;
    print "  [PASS] {$description}\n";
  }
  else {
    $failed++;
    print "  [FAIL] {$description}\n";
  }
}

print "============================================================\n";
print " AI Content Audit Scoring Improvements Test Suite\n";
print "============================================================\n\n";

// ---------------------------------------------------------------------------
// 1. Smooth Age Decay
// ---------------------------------------------------------------------------
print "--- 1. Testing Smooth Age Decay ---\n";

$now = REQUEST_TIME;
$day = 86400;
$month = 30 * $day;

// Node updated 2 months ago (within 6 month decay start threshold)
$changed_fresh = $now - (2 * $month);
$age_score_fresh = ai_content_audit_node_age_score($changed_fresh, 24, 6);
test_assert($age_score_fresh === 0.0, "Fresh node (2 months old) has age score 0.0 (got {$age_score_fresh})");
test_assert(abs(ai_content_audit_node_age_months($changed_fresh) - 2.0) < 0.02, "Actual age reports 2.0 months (got " . ai_content_audit_node_age_months($changed_fresh) . ")");

// Node updated 30 months ago (beyond 24 month stale threshold)
$changed_stale = $now - (30 * $month);
$age_score_stale = ai_content_audit_node_age_score($changed_stale, 24, 6);
test_assert($age_score_stale === 1.0, "Stale node (30 months old) has age score 1.0 (got {$age_score_stale})");
test_assert(abs(ai_content_audit_node_age_months($changed_stale) - 30.0) < 0.02, "Actual age reports 30.0 months (got " . ai_content_audit_node_age_months($changed_stale) . ")");
$created_legacy = $now - (150 * $month);
test_assert(abs(ai_content_audit_node_created_age_months($created_legacy) - 150.0) < 0.02, "Node age reports 150.0 months from creation (got " . ai_content_audit_node_created_age_months($created_legacy) . ")");

// Node updated exactly midway: 15 months old.
// Decay range is 6 to 24 months (span = 18 months). (15 - 6) / 18 = 0.50.
$changed_mid = $now - (15 * $month);
$age_score_mid = ai_content_audit_node_age_score($changed_mid, 24, 6);
test_assert(abs($age_score_mid - 0.50) < 0.02, "Mid-age node (15 months old) has age score ~0.50 (got {$age_score_mid})");

// Cluster of 3 nodes: fresh (0.0), mid (0.5), stale (1.0)
$cluster_nodes = [
  1 => ['nid' => 1, 'changed' => $changed_fresh],
  2 => ['nid' => 2, 'changed' => $changed_mid],
  3 => ['nid' => 3, 'changed' => $changed_stale],
];
$cluster_age = ai_content_audit_age_score($cluster_nodes, 24, 6);
test_assert(abs($cluster_age - 0.50) < 0.02, "Cluster age score is the average smooth decay of all members (~0.50, got {$cluster_age})");

// Empty node list
$empty_age = ai_content_audit_age_score([]);
test_assert($empty_age === 0.0, "Empty node list produces age score 0.0");

// ---------------------------------------------------------------------------
// 2. Smarter Canonical Selection (Multi-Factor)
// ---------------------------------------------------------------------------
print "\n--- 2. Testing Smarter Canonical Selection ---\n";

// Scenario A: Stale stub updated yesterday vs comprehensive pillar page updated 8 months ago.
// In the old logic, the stub won because changed > best_changed.
$stub_nid = 101;
$pillar_nid = 102;

$node_rows_substantive = [
  $stub_nid => [
    'nid' => $stub_nid,
    'title' => 'Sample Product Overview (Short Stub)',
    'type' => 'post',
    'created' => $now - (300 * $day),
    'changed' => $now - (1 * $day), // Changed yesterday!
  ],
  $pillar_nid => [
    'nid' => $pillar_nid,
    'title' => 'Comprehensive Guide to Product',
    'type' => 'post',
    'created' => $now - (300 * $day),
    'changed' => $now - (240 * $day), // 8 months ago
  ],
];

// Stub has 120 chars, pillar has 4500 chars. No traffic data.
$lengths = [
  $stub_nid => 120,
  $pillar_nid => 4500,
];
$picked = ai_content_audit_pick_canonical($node_rows_substantive, [], $lengths);
test_assert($picked === $pillar_nid, "Comprehensive pillar page (4,500 chars) wins canonical over 120-char stub edited yesterday");

// Scenario B: High traffic vs zero traffic page.
$traffic_test_a = 201;
$traffic_test_b = 202;
$node_rows_traffic = [
  $traffic_test_a => [
    'nid' => $traffic_test_a,
    'title' => 'Main Landing Page',
    'type' => 'page',
    'created' => $now - (500 * $day),
    'changed' => $now - (100 * $day),
  ],
  $traffic_test_b => [
    'nid' => $traffic_test_b,
    'title' => 'Internal Duplicate Draft',
    'type' => 'page',
    'created' => $now - (50 * $day),
    'changed' => $now - (10 * $day),
  ],
];
$traffic_counts = [
  $traffic_test_a => 8500, // 8,500 pageviews
  $traffic_test_b => 12,   // 12 pageviews
];
$lengths_similar = [
  $traffic_test_a => 1200,
  $traffic_test_b => 1150,
];
$picked_traffic = ai_content_audit_pick_canonical($node_rows_traffic, $traffic_counts, $lengths_similar);
test_assert($picked_traffic === $traffic_test_a, "High-traffic page (8,500 views) wins canonical over duplicate with 12 views");

// Scenario C: Single node cluster
$single_picked = ai_content_audit_pick_canonical([999 => ['nid' => 999, 'changed' => $now]]);
test_assert($single_picked === 999, "Single node cluster returns that node");

// ---------------------------------------------------------------------------
// 3. Dynamic Weight Re-Normalization & Configurable Weights
// ---------------------------------------------------------------------------
print "\n--- 3. Testing Dynamic Priority Weights ---\n";

// When traffic is present: weights should sum to 1.0 (50% / 30% / 20%)
$weights_with_traffic = ai_content_audit_get_priority_weights(TRUE);
test_assert(abs(($weights_with_traffic['similarity'] + $weights_with_traffic['age'] + $weights_with_traffic['traffic']) - 1.0) < 0.001, "Weights with traffic sum to 1.0");
test_assert($weights_with_traffic['traffic'] > 0, "Traffic weight is active when traffic exists");

// When traffic is NOT present: traffic weight is 0.0, and remaining weights re-normalize to 1.0
$weights_no_traffic = ai_content_audit_get_priority_weights(FALSE);
test_assert($weights_no_traffic['traffic'] === 0.0, "Traffic weight is 0.0 when traffic is missing");
test_assert(abs(($weights_no_traffic['similarity'] + $weights_no_traffic['age']) - 1.0) < 0.001, "Remaining weights re-normalize to 1.0 when traffic is missing");

// Verify that a fresh cluster with high similarity (0.80) does not get misclassified as 'trivial'
$norm_composite_no_traffic = round((0.80 * $weights_no_traffic['similarity']) + (0.0 * $weights_no_traffic['age']), 4);
test_assert($norm_composite_no_traffic >= 0.35, "Re-normalized fresh cluster score ({$norm_composite_no_traffic}) exceeds trivial threshold (>= 0.35)");

$rot_verdict = ai_content_audit_rot_classify(0.80, 0.0, $norm_composite_no_traffic);
test_assert($rot_verdict !== 'trivial', "Fresh content is not misclassified as trivial (classified as: '{$rot_verdict}')");

// Related but distinct content must remain a human-review item even when all
// members are old. Age alone is not evidence that pages should be refreshed or
// consolidated.
$related_old_verdict = ai_content_audit_rot_classify(0.80, 1.0, 0.90);
test_assert($related_old_verdict === 'review', "Related old content at 0.80 similarity remains review (got '{$related_old_verdict}')");
test_assert(ai_content_audit_recommend_action(0.80, 1.0, 0.90) === 'review', "Related old content does not receive an automatic action recommendation");

// Stronger similarity can become actionable, with pruning reserved for the
// highest-confidence matches.
$strong_old_verdict = ai_content_audit_rot_classify(0.86, 1.0, 0.90);
test_assert($strong_old_verdict === 'redundant_outdated', "Strongly similar old content is classified redundant and outdated");
test_assert(ai_content_audit_recommend_action(0.86, 1.0, 0.90) === 'merge', "Strong but sub-0.90 similarity recommends merge rather than prune");
test_assert(ai_content_audit_recommend_action(0.90, 1.0, 0.90) === 'prune', "0.90 similarity is required for a prune recommendation");

// Per-page scoring uses similarity as evidence for redundancy, while age and
// content length remain independent ROT signals.
test_assert(ai_content_audit_node_rot_classify(0.86, 0.0, 1200) === 'redundant', "Fresh near-duplicate page is classified redundant");
test_assert(ai_content_audit_node_rot_classify(0.80, 1.0, 1200) === 'outdated', "Old related page below redundancy threshold is classified outdated");
test_assert(ai_content_audit_node_rot_classify(0.0, 0.0, 120) === 'trivial', "Fresh short page is classified trivial");
test_assert(ai_content_audit_node_rot_classify(0.0, 0.0, 1200) === 'review', "Fresh substantial page remains review");
test_assert(ai_content_audit_node_recommend_action('trivial', FALSE, 0.0, 0.0) === 'review', "Fresh trivial page remains review-only");
test_assert(ai_content_audit_node_recommend_action('trivial', FALSE, 0.0, 1.0, 150.0, 0.0) === 'retire', "Fully stale legacy trivial page receives a retire recommendation");
test_assert(ai_content_audit_node_recommend_action('redundant_outdated', FALSE, 0.95, 1.0) === 'review', "Semantic redundancy remains review-only");
test_assert(ai_content_audit_node_recommend_action('outdated', FALSE, 0.0, 1.0, 150.0, 0.0) === 'retire', "Fully stale 12.5-year-old zero-traffic legacy page receives a retire recommendation");
test_assert(ai_content_audit_node_recommend_action('outdated', FALSE, 0.0, 1.0, 150.0, 0.10) === 'refresh', "Legacy page with meaningful traffic remains refresh-only");
test_assert(ai_content_audit_node_recommend_action('outdated', FALSE, 0.0, 1.0, 100.0, 0.0) === 'refresh', "Fully stale page under the legacy-age threshold remains refresh-only");

// ---------------------------------------------------------------------------
// 4. Rich Text Excerpt Construction for RAG Searches
// ---------------------------------------------------------------------------
print "\n--- 4. Testing RAG Query Snippet Generation ---\n";

$snippet_empty = ai_content_audit_get_node_text_snippet(99999999, 'Test Headline Only');
test_assert($snippet_empty === 'Test Headline Only', "Fallback to title only when no body exists");

// Test with existing node from AMA Foundation site if present
$sample_node = db_query('SELECT n.nid, n.title, b.body_value FROM {node} n INNER JOIN {field_data_body} b ON b.entity_id = n.nid WHERE CHAR_LENGTH(b.body_value) > 50 LIMIT 1')->fetchAssoc();
if ($sample_node) {
  $snippet_enriched = ai_content_audit_get_node_text_snippet($sample_node['nid'], $sample_node['title'], 200);
  test_assert(strpos($snippet_enriched, $sample_node['title']) === 0, "Generated snippet starts with title");
  test_assert(strlen($snippet_enriched) > strlen($sample_node['title']), "Generated snippet appends body content");
  test_assert(strpos($snippet_enriched, '<') === FALSE, "HTML tags are stripped from query snippet");
}
else {
  test_assert(TRUE, "Skipped live body query (no body rows in test db)");
}

// Paragraph-backed content must also enrich the scan query. This is the
// storage pattern used by the site's article content types.
$paragraph_sample = db_query(
  "SELECT p.entity_id, n.title
     FROM {field_data_field_paragraphs} p
     INNER JOIN {node} n ON n.nid = p.entity_id
    WHERE p.entity_type = 'node' AND p.deleted = 0
    LIMIT 1"
)->fetchAssoc();
if ($paragraph_sample) {
  $paragraph_snippet = ai_content_audit_get_node_text_snippet($paragraph_sample['entity_id'], $paragraph_sample['title'], 200);
  test_assert(strlen($paragraph_snippet) > strlen($paragraph_sample['title']), "Paragraph-backed node snippet includes editable paragraph text");
}
else {
  test_assert(TRUE, "Skipped paragraph-backed snippet query (no paragraph nodes in test db)");
}

// Semantic similarity and title overlap are not duplicate evidence. These two
// known related articles have different bodies and must not share a fingerprint.
$distinct_article_fingerprints = [];
foreach ([33723, 33775] as $nid) {
  $fingerprint = ai_content_audit_get_node_audit_fingerprint($nid);
  if ($fingerprint !== '') {
    $distinct_article_fingerprints[] = $fingerprint;
  }
}
if (count($distinct_article_fingerprints) === 2) {
  test_assert(count(array_unique($distinct_article_fingerprints)) === 2, "Distinct related articles do not receive the same exact-content fingerprint");
}
else {
  test_assert(TRUE, "Skipped live exact-fingerprint comparison (fixture articles unavailable)");
}

// Every scanned node receives a ROT assessment even when there are no exact
// duplicate clusters to display.
if (db_table_exists('ai_content_audit_node_score')) {
  $latest_rot_run = db_query(
    "SELECT run_id, node_count FROM {ai_content_audit_run} WHERE status = 'complete' ORDER BY started DESC LIMIT 1"
  )->fetchAssoc();
  if ($latest_rot_run && (int) $latest_rot_run['node_count'] > 0) {
    $rot_count = (int) db_query(
      'SELECT COUNT(*) FROM {ai_content_audit_node_score} WHERE run_id = :rid',
      [':rid' => $latest_rot_run['run_id']]
    )->fetchField();
    test_assert($rot_count === (int) $latest_rot_run['node_count'], "Latest scan stores one ROT assessment per scanned node ({$rot_count}/{$latest_rot_run['node_count']})");
    $distinct_priorities = (int) db_query(
      'SELECT COUNT(DISTINCT composite_score) FROM {ai_content_audit_node_score} WHERE run_id = :rid',
      [':rid' => $latest_rot_run['run_id']]
    )->fetchField();
    test_assert($distinct_priorities > 1, "ROT priority scores provide sortable variation ({$distinct_priorities} distinct values)");
  }
  else {
    test_assert(TRUE, "Skipped per-node ROT count (no completed scan with nodes)");
  }
}
else {
  test_assert(TRUE, "Skipped per-node ROT count (assessment table unavailable)");
}

// ---------------------------------------------------------------------------
// 5. Fallback Clustering Actual Jaccard Scoring
// ---------------------------------------------------------------------------
print "\n--- 5. Testing Fallback Jaccard Clustering Calculation ---\n";

$mock_nodes = [
  1 => ['nid' => 1, 'title' => 'Backdrop CMS Modern Content Audit System'],
  2 => ['nid' => 2, 'title' => 'Backdrop CMS Modern Content Audit Architecture Guide'],
];
$fallback_res = ai_content_audit_fallback_clusters($mock_nodes);
test_assert(!empty($fallback_res), "Fallback clustering groups matching titles");
if (!empty($fallback_res)) {
  $sim = $fallback_res[0]['similarity_score'];
  test_assert($sim > 0.0 && $sim <= 1.0, "Similarity score is a computed float (got {$sim})");
  test_assert($sim !== 0.5, "Similarity score is calculated from real word overlap rather than hardcoded 0.5 (got {$sim})");
}

// ---------------------------------------------------------------------------
// 6. Settings Form Validation
// ---------------------------------------------------------------------------
print "\n--- 6. Testing Settings Form Validation ---\n";

$form = [];
$form_state_valid = [
  'values' => [
    'weight_similarity' => 50,
    'weight_age' => 30,
    'weight_traffic' => 20,
    'decay_start_months' => 6,
    'stale_age_months' => 24,
  ],
];
form_clear_error();
ai_content_audit_settings_form_validate($form, $form_state_valid);
test_assert(!form_get_errors(), "Valid weights and decay thresholds pass validation");

$form_state_invalid = [
  'values' => [
    'weight_similarity' => -5,
    'weight_age' => 0,
    'weight_traffic' => 0,
    'decay_start_months' => 30,
    'stale_age_months' => 20,
  ],
];
form_clear_error();
ai_content_audit_settings_form_validate($form, $form_state_invalid);
$errors = form_get_errors();
test_assert(!empty($errors['weight_similarity']) && !empty($errors['decay_start_months']), "Negative weights and inverted thresholds are caught and flagged");
form_clear_error();

// ---------------------------------------------------------------------------
// 7. Cluster Priority Re-Ranking
// ---------------------------------------------------------------------------
print "\n--- 7. Testing Cluster Re-Ranking on Existing Run ---\n";

$run = db_query('SELECT run_id FROM {ai_content_audit_run} WHERE cluster_count > 0 LIMIT 1')->fetchField();
if ($run) {
  ai_content_audit_rank_clusters($run);
  $sample_cluster = db_query('SELECT cid, topic_label, rot_type, composite_score FROM {ai_content_audit_cluster} WHERE run_id = :rid LIMIT 1', [':rid' => $run])->fetchAssoc();
  test_assert(!empty($sample_cluster), "Re-ranked clusters in run {$run}");
  test_assert($sample_cluster['composite_score'] > 0.0, "Re-ranked cluster has valid composite priority score ({$sample_cluster['composite_score']})");
}
else {
  test_assert(TRUE, "No existing run to re-rank (skipped)");
}

print "\n============================================================\n";
print " Test Results: {$passed} Passed, {$failed} Failed\n";
print "============================================================\n";

if ($failed > 0) {
  exit(1);
}
