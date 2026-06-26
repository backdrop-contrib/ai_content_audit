<?php

/**
 * @file
 * Field-level helpers for AI Content Audit.
 *
 * Pure functions that read or write text-bearing fields on a node or its
 * attached paragraph items. Extracted from ai_content_audit.tools.inc so the
 * LLM auto-draft pipeline (llm.inc) can consume them without pulling in the
 * full agent-tool surface.
 *
 * Both writers (set_text_field_value, set_paragraph_target_value) are entity-
 * type aware: passing 'node' for a paragraphs_item makes field_language() →
 * entity_extract_ids() throw EntityMalformedException, which would otherwise
 * silently break every paragraph-targeted AI proposal.
 *
 * Used by:
 *   - tools.inc (audit_get_node_content, audit_propose_edit execute callbacks)
 *   - llm.inc   (auto-draft canonical edit pipeline)
 */

/**
 * Normalize rich text into comparable plain text.
 */
function ai_content_audit_normalize_text_for_compare($text) {
  $text = is_string($text) ? $text : '';
  $text = strip_tags($text);
  $text = html_entity_decode($text, ENT_QUOTES, 'UTF-8');
  $text = preg_replace('/\s+/u', ' ', $text);
  return trim($text);
}

/**
 * Collect editable text fields from a node (or paragraph entity).
 *
 * Pass $entity_type 'paragraphs_item' for paragraph items — they carry their
 * bundle in $entity->bundle, not $entity->type, so treating them as nodes
 * silently returns no fields.
 */
function ai_content_audit_collect_editable_text_fields($node, $entity_type = 'node') {
  $fields = [];
  list(, , $bundle) = entity_extract_ids($entity_type, $node);
  $field_instances = field_info_instances($entity_type, $bundle);

  foreach ($field_instances as $field_name => $instance) {
    $field_info = field_info_field($field_name);
    if (empty($field_info) || !in_array($field_info['type'], ['text', 'text_long', 'text_with_summary'], TRUE)) {
      continue;
    }
    $lang = field_language($entity_type, $node, $field_name);
    if (!isset($lang) || $lang === FALSE) {
      $lang = LANGUAGE_NONE;
    }
    if (empty($node->{$field_name}[$lang][0]['value'])) {
      continue;
    }
    $fields[$field_name] = $node->{$field_name}[$lang][0]['value'];
    if ($field_info['type'] === 'text_with_summary') {
      $fields[$field_name . '__format'] = isset($node->{$field_name}[$lang][0]['format']) ? $node->{$field_name}[$lang][0]['format'] : '';
      $fields[$field_name . '__summary'] = isset($node->{$field_name}[$lang][0]['summary']) ? $node->{$field_name}[$lang][0]['summary'] : '';
    }
  }

  return $fields;
}

/**
 * Collect editable text targets from paragraph items attached to a node.
 */
function ai_content_audit_collect_paragraph_text_targets($node) {
  if (!module_exists('paragraphs')) {
    return [];
  }

  $targets = [];
  $field_instances = field_info_instances('node', $node->type);
  foreach ($field_instances as $field_name => $instance) {
    $field_info = field_info_field($field_name);
    if (empty($field_info) || $field_info['type'] !== 'paragraphs') {
      continue;
    }
    $lang = field_language('node', $node, $field_name);
    if (!isset($lang) || $lang === FALSE) {
      $lang = LANGUAGE_NONE;
    }
    if (empty($node->{$field_name}[$lang]) || !is_array($node->{$field_name}[$lang])) {
      continue;
    }

    foreach ($node->{$field_name}[$lang] as $delta => $item) {
      if (empty($item['revision_id'])) {
        continue;
      }
      $paragraph = paragraphs_item_revision_load((int) $item['revision_id']);
      if (!$paragraph) {
        continue;
      }
      $paragraph_fields = ai_content_audit_collect_editable_text_fields($paragraph, 'paragraphs_item');
      if (empty($paragraph_fields)) {
        continue;
      }
      $primary_field = ai_content_audit_pick_primary_edit_field($paragraph, $paragraph_fields, 'paragraphs_item');
      if (!$primary_field || empty($paragraph_fields[$primary_field])) {
        continue;
      }
      $targets[] = [
        'target_type' => 'paragraph_field',
        'host_field' => $field_name,
        'host_langcode' => $lang,
        'delta' => (int) $delta,
        'item_id' => (int) $paragraph->item_id,
        'revision_id' => (int) $paragraph->revision_id,
        'paragraph_bundle' => $paragraph->bundle,
        'field_name' => $primary_field,
        'value' => $paragraph_fields[$primary_field],
        'summary' => isset($paragraph_fields[$primary_field . '__summary']) ? $paragraph_fields[$primary_field . '__summary'] : '',
      ];
    }
  }

  return $targets;
}

/**
 * Pick the primary editable text field for a node.
 */
function ai_content_audit_pick_primary_edit_field($node, array $fields = [], $entity_type = 'node') {
  if (empty($fields)) {
    $fields = ai_content_audit_collect_editable_text_fields($node, $entity_type);
  }
  if (isset($fields['body'])) {
    return 'body';
  }

  $best_field = '';
  $best_length = 0;
  foreach ($fields as $field_name => $value) {
    if (in_array($field_name, ['body_format', 'body_summary'], TRUE) || !is_string($value)) {
      continue;
    }
    $length = backdrop_strlen(trim($value));
    if ($length > $best_length) {
      $best_length = $length;
      $best_field = $field_name;
    }
  }

  return $best_field;
}

/**
 * Pick the best editable target for a node or one of its paragraphs.
 */
function ai_content_audit_pick_primary_edit_target($node, array $fields = [], array $paragraph_targets = []) {
  if (empty($fields)) {
    $fields = ai_content_audit_collect_editable_text_fields($node);
  }
  if (empty($paragraph_targets)) {
    $paragraph_targets = ai_content_audit_collect_paragraph_text_targets($node);
  }

  $node_field = ai_content_audit_pick_primary_edit_field($node, $fields);
  if ($node_field) {
    return [
      'target_type' => 'node_field',
      'field_name' => $node_field,
      'value' => isset($fields[$node_field]) ? $fields[$node_field] : '',
      'summary' => isset($fields[$node_field . '__summary']) ? $fields[$node_field . '__summary'] : '',
    ];
  }

  $preferred = [];
  foreach ($paragraph_targets as $target) {
    if ($target['paragraph_bundle'] === 'body' && $target['field_name'] === 'field_body_paragraph') {
      return $target;
    }
    $preferred[] = $target;
  }

  if (empty($preferred)) {
    return [];
  }

  usort($preferred, function ($a, $b) {
    return backdrop_strlen($b['value']) - backdrop_strlen($a['value']);
  });

  return reset($preferred);
}

/**
 * Set a text field value on an entity.
 *
 * Used for both node fields and paragraphs_item fields. The entity_type
 * argument is required for the latter — passing 'node' for a paragraphs_item
 * makes field_language() → entity_extract_ids() throw EntityMalformedException
 * because the paragraph object doesn't carry the node bundle key.
 */
function ai_content_audit_set_text_field_value(&$entity, $field_name, $value, $summary = NULL, $entity_type = 'node') {
  if (empty($field_name) || empty($entity->{$field_name})) {
    $entity->{$field_name} = [];
  }
  $lang = field_language($entity_type, $entity, $field_name);
  if (!isset($lang) || $lang === FALSE) {
    $lang = LANGUAGE_NONE;
  }

  $field_info = field_info_field($field_name);
  if (empty($field_info) || !in_array($field_info['type'], ['text', 'text_long', 'text_with_summary'], TRUE)) {
    return FALSE;
  }

  if (empty($entity->{$field_name}[$lang][0]) || !is_array($entity->{$field_name}[$lang][0])) {
    $entity->{$field_name}[$lang][0] = [];
  }

  $entity->{$field_name}[$lang][0]['value'] = $value;
  if ($field_info['type'] === 'text_with_summary' && $summary !== NULL) {
    $entity->{$field_name}[$lang][0]['summary'] = $summary;
  }

  return TRUE;
}

/**
 * Apply a proposed change to a paragraph-backed text field on a node.
 *
 * Loads the paragraph at its current revision, writes the new text, and
 * stages a new paragraph revision (saved when the host node is saved). The
 * caller must call node_save() afterwards to actually persist the new
 * paragraph + node revision.
 */
function ai_content_audit_set_paragraph_target_value(&$node, array $target_context, $value, $summary = NULL) {
  if (
    empty($target_context['host_field']) ||
    !isset($target_context['delta']) ||
    empty($target_context['revision_id']) ||
    empty($target_context['field_name'])
  ) {
    return FALSE;
  }

  $paragraph = paragraphs_item_revision_load((int) $target_context['revision_id']);
  if (!$paragraph) {
    return FALSE;
  }

  if (!ai_content_audit_set_text_field_value($paragraph, $target_context['field_name'], $value, $summary, 'paragraphs_item')) {
    return FALSE;
  }

  $paragraph->revision = TRUE;
  $paragraph->is_new_revision = TRUE;
  $host_field = $target_context['host_field'];
  $lang = !empty($target_context['host_langcode']) ? $target_context['host_langcode'] : LANGUAGE_NONE;
  $delta = (int) $target_context['delta'];

  if (!isset($node->{$host_field}[$lang])) {
    $node->{$host_field}[$lang] = [];
  }

  $node->{$host_field}[$lang][$delta] = [
    'entity' => $paragraph,
    'value' => $paragraph->item_id,
    'revision_id' => $paragraph->revision_id,
  ];

  return TRUE;
}
