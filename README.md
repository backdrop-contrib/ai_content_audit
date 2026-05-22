# AI Content Audit

AI Content Audit identifies redundant, outdated, and trivial (ROT) content on
your Backdrop CMS site by clustering semantically similar nodes and scoring them
for human review. No content is changed until a site administrator approves each
proposed action.

## Features

- **Semantic clustering** — groups nodes by topic similarity using vector
  embeddings from your configured AI search index.
- **ROT classification** — automatically labels clusters as redundant,
  outdated, trivial, or a combination based on similarity and age scores.
- **Composite scoring** — combines similarity, traffic (from uploaded GA4 CSV
  data), and age into a single priority score for each cluster.
- **LLM-generated summaries** — optionally asks an AI model to summarize why
  each cluster is flagged and what action it recommends.
- **Automatic draft edit proposals** — optionally asks an AI model to draft
  updates on canonical pages for merge/refresh clusters and saves them as
  approval-gated revisions.
- **Action queue with approval gate** — proposed actions (prune, merge,
  flag for review, proposed edits) are queued for administrator approval before
  any node is touched.
- **Draft edit proposals** — an AI agent can save proposed text edits as a
  draft revision so admins can review a diff and approve or reject.
- **Incremental batch scanning** — large sites are scanned in configurable
  batches, resuming across cron runs.
- **Automated scheduling** — optionally runs a full scan on a configurable
  cron interval.
- **AI agent tool integration** — exposes a set of structured tools so an AI
  agent (via the AI Agents module) can query clusters, propose actions, and
  read node content programmatically.

## Requirements

- Backdrop CMS 1.x
- [AI module](https://github.com/backdrop-contrib/ai) with at least one
  configured text provider
- [AI Search](https://github.com/backdrop-contrib/ai) with a configured vector
  index (required for embedding-based clustering)

## Installation

1. Install and enable the AI module and configure a text provider.
2. Configure an AI Search vector index with your content indexed.
3. Install and enable this module.
4. Visit **Admin → Configuration → AI → AI Content Audit** to configure.

## Configuration

Navigate to **Admin → Configuration → AI → AI Content Audit → Settings**.

Key settings:

- **AI Search index** — the vector index used to compute node similarity.
- **Similarity threshold** — minimum score for two nodes to be considered
  related (0.0–1.0; default 0.75).
- **Cron scanning** — enable automatic scans and set the interval and batch
  size.
- **LLM summary model** — model used to generate cluster summaries (can be
  left empty to skip summaries).
- **Automatic draft edits** — optional post-scan AI drafting for canonical
  pages in merge/refresh clusters, with a model choice and per-run cap.
- **Traffic data** — upload a GA4 pageviews CSV to factor traffic into cluster
  scoring. Nodes with high traffic score lower for pruning.

## Usage

### Running a Scan

Go to **Admin → Configuration → AI → AI Content Audit**. Click **Scan Now**
to start an incremental scan. Progress is shown on the dashboard. Large sites
may require multiple cron runs to complete.

The scan produces **clusters** — groups of nodes that are semantically similar.
Each cluster gets a composite score and a recommended action.

If automatic draft edits are enabled, the module can also stage draft
revisions on canonical pages for high-value merge/refresh clusters. These are
saved as `edit_proposed` items with `pending` status and still require
administrator approval.

### Reviewing Clusters

Go to **Clusters** to see all identified clusters ordered by composite score.
Click a cluster to see:

- All member nodes with their individual scores.
- Which node is the canonical (best) version.
- The LLM-generated summary (if configured).
- Recommended action for each node.

### Approving Actions

Actions proposed by the scan (or by an AI agent) appear in the **Action Queue**
tab. This includes pending `edit_proposed` draft revisions, which can be
reviewed through their diff links before approval. Approved actions are
executed automatically on the next queue processing run.

Possible actions per candidate node:

| Action | Effect |
|---|---|
| `prune` | Unpublishes the node. |
| `merge` | Redirects the node to the canonical URL (requires Redirect module). |
| `flag_for_review` | Adds a watchdog notice; no structural change. |
| `edit_proposed` | Applies a saved draft revision after admin approval. |
| `ignored` | Records the decision; no change to the node. |

## AI Agent Tools

When used alongside the AI Agents module, AI Content Audit exposes the
following tools for agent use:

| Tool | Description |
|---|---|
| `audit_list_clusters` | List clusters from the latest scan, filtered by state. |
| `audit_get_cluster` | Get full details for one cluster including candidate scores. |
| `audit_list_runs` | List recent scan runs with status and counts. |
| `audit_trigger_scan` | Trigger a scan batch immediately. |
| `audit_propose_action` | Propose an action for a candidate (queued for approval). |
| `audit_get_node_content` | Read current node text and any AI Content Lifecycle analysis. |
| `audit_propose_edit` | Save proposed text edits as a draft revision for admin review. |

All write operations (`audit_propose_action`, `audit_propose_edit`) are staged
for approval in the queue — the agent cannot apply changes directly.

## Issues

Bugs and feature requests should be reported in the
[Issue Queue](https://github.com/backdrop-contrib/ai-content-audit/issues).

## Current Maintainer

[Justin Keiser](https://github.com/keiserjb)

## Credits

- Created for Backdrop CMS by [Justin Keiser](https://github.com/keiserjb).
- Developed with AI assistance.

## License

This project is GPL v2 software. See the LICENSE.txt file in this directory for
complete text.
