<?php

/**
 * @file
 * Apply approved candidate actions (prune, merge, edit_proposed, flag).
 *
 * Extracted from ai_content_audit.scanner.inc. Loaded on demand by
 * ai_content_audit_apply_approved() in the .module file. Pure execution —
 * no scanning, clustering, scoring, or LLM calls here.
 */

/**
 * Execute approved candidate actions. Returns count applied.
 */
function ai_content_audit_execute_approved($limit = 5) {
  $query = db_select('ai_content_audit_candidate', 'ca');
  $query->fields('ca');
  $query->condition('status', 'approved');
  $query->orderBy('approved_at', 'ASC');
  $query->range(0, (int) $limit);
  $candidates = $query->execute()->fetchAllAssoc('acid', PDO::FETCH_ASSOC);

  $applied = 0;
  foreach ($candidates as $candidate) {
    $success = ai_content_audit_apply_candidate($candidate);
    if ($success) {
      db_update('ai_content_audit_candidate')->fields([
        'status'     => 'applied',
        'applied_at' => REQUEST_TIME,
        'updated'    => REQUEST_TIME,
      ])->condition('acid', $candidate['acid'])->execute();
      $applied++;
    }
    else {
      db_update('ai_content_audit_candidate')->fields([
        'status'  => 'failed',
        'updated' => REQUEST_TIME,
      ])->condition('acid', $candidate['acid'])->execute();
    }
  }

  return $applied;
}

/**
 * Apply a single approved candidate action.
 */
function ai_content_audit_apply_candidate(array $candidate) {
  $nid = (int) $candidate['nid'];
  $task_type = $candidate['task_type'];

  if (!$nid || $task_type === 'canonical') {
    return FALSE;
  }

  $node = node_load($nid);
  if (!$node) {
    db_update('ai_content_audit_candidate')->fields([
      'error_message' => "Node $nid not found.",
    ])->condition('acid', $candidate['acid'])->execute();
    return FALSE;
  }

  try {
    switch ($task_type) {
      case 'edit_proposed':
        // Promote the saved forward revision through Draft Workflow.
        $vids = !empty($candidate['before_state']) ? json_decode($candidate['before_state'], TRUE) : [];
        $proposed_vid = !empty($vids['proposed_vid']) ? (int) $vids['proposed_vid'] : 0;
        if ($proposed_vid) {
          $proposed = node_load($node->nid, $proposed_vid);
          if ($proposed) {
            if (module_exists('draft_workflow') && method_exists($proposed, 'setIsActiveRevision')) {
              $proposed->status = $node->status;
              $proposed->setIsActiveRevision();
              $proposed->save();
            }
            else {
              $proposed->status = 1;
              $proposed->revision = TRUE;
              $proposed->is_new_revision = TRUE;
              $proposed->log = 'AI proposed edit approved.';
              node_save($proposed);
              // Delete the old unpublished draft to avoid clutter.
              if (function_exists('node_revision_delete')) {
                node_revision_delete($proposed_vid);
              }
            }
            return TRUE;
          }
        }
        return FALSE;

      case 'duplicate_candidate':
      case 'prune':
        $node->status = 0;
        node_save($node);
        return TRUE;

      case 'flag_for_review':
      case 'refresh':
      case 'merge':
      case 'review':
        if (module_exists('ai_content_lifecycle') && db_table_exists('content_lifecycle')) {
          $existing = db_query(
            'SELECT id FROM {content_lifecycle} WHERE entity_type = :type AND entity_id = :id LIMIT 1',
            [':type' => 'node', ':id' => $nid]
          )->fetchField();
          if ($existing) {
            db_update('content_lifecycle')->fields([
              'review_status' => 'needs_review',
              'changed'       => REQUEST_TIME,
            ])->condition('id', $existing)->execute();
          }
          else {
            $lifecycle = entity_create('content_lifecycle', [
              'entity_type'   => 'node',
              'entity_bundle' => $node->type,
              'entity_id'     => $nid,
              'label'         => $node->title,
              'review_status' => 'needs_review',
            ]);
            entity_save('content_lifecycle', $lifecycle);
          }
        }
        return TRUE;

      default:
        return FALSE;
    }
  }
  catch (\Exception $e) {
    db_update('ai_content_audit_candidate')->fields([
      'error_message' => substr($e->getMessage(), 0, 512),
    ])->condition('acid', $candidate['acid'])->execute();
    return FALSE;
  }
}
