<?php

/**
 * @file
 * Cluster scoring and ROT classification.
 *
 * Extracted from ai_content_audit.scanner.inc. Pure functions over already-
 * built clusters: pick canonical, age score, traffic score, composite rank,
 * ROT classification, recommended action. No clustering or LLM here.
 */

/**
 * Pick canonical node: most recently changed wins.
 */
function ai_content_audit_pick_canonical(array $node_rows) {
  $best = NULL;
  $best_changed = -1;
  foreach ($node_rows as $nid => $row) {
    $changed = isset($row['changed']) ? (int) $row['changed'] : 0;
    if ($changed > $best_changed) {
      $best_changed = $changed;
      $best = $nid;
    }
  }
  return $best ? (int) $best : 0;
}

/**
 * Score cluster age: 0 = all recent, 1 = all stale (older than 2 years).
 */
function ai_content_audit_age_score(array $node_rows) {
  if (empty($node_rows)) {
    return 0.0;
  }
  $two_years = 60 * 60 * 24 * 365 * 2;
  $stale = 0;
  foreach ($node_rows as $row) {
    $changed = isset($row['changed']) ? (int) $row['changed'] : 0;
    if ($changed && (REQUEST_TIME - $changed) > $two_years) {
      $stale++;
    }
  }
  return round($stale / count($node_rows), 2);
}

/**
 * Classify a cluster using the ROT framework.
 *
 * - Redundant: near-identical content, both relatively recent.
 * - Outdated: similar topic but significant age gap between pages.
 * - Trivial: loosely related, low composite priority.
 *
 * Returns a string: 'redundant', 'outdated', 'trivial',
 * 'redundant_outdated', or 'review'.
 */
function ai_content_audit_rot_classify($similarity_score, $age_score, $composite_score = 0.0) {
  $is_similar = $similarity_score >= 0.85;
  $is_old     = $age_score >= 0.5;
  $is_trivial = $composite_score < 0.35 && !$is_similar;

  if ($is_trivial) {
    return 'trivial';
  }
  if ($is_similar && $is_old) {
    return 'redundant_outdated';
  }
  if ($is_similar) {
    return 'redundant';
  }
  if ($is_old) {
    return 'outdated';
  }
  return 'review';
}

/**
 * Derive recommended action from ROT classification.
 */
function ai_content_audit_recommend_action($similarity_score, $age_score, $composite_score = 0.0) {
  $rot = ai_content_audit_rot_classify($similarity_score, $age_score, $composite_score);
  switch ($rot) {
    case 'redundant':
      return $similarity_score >= 0.9 ? 'prune' : 'merge';
    case 'redundant_outdated':
      return 'prune';
    case 'outdated':
      return 'refresh';
    case 'trivial':
      return 'review';
    default:
      return 'review';
  }
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
 * Update composite scores for clusters in a run.
 */
function ai_content_audit_rank_clusters($run_id) {
  $result = db_query(
    'SELECT cid, similarity_score, traffic_score, age_score FROM {ai_content_audit_cluster} WHERE run_id = :rid',
    [':rid' => $run_id]
  );
  foreach ($result as $row) {
    $composite = round(
      ($row->similarity_score * 0.5) + ($row->age_score * 0.3) + ($row->traffic_score * 0.2),
      4
    );
    db_update('ai_content_audit_cluster')
      ->fields(['composite_score' => $composite])
      ->condition('cid', $row->cid)
      ->execute();
  }
}
