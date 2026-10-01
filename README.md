# AI Content Audit

AI Content Audit scores every published page for Redundant, Outdated, and
Trivial (ROT) content, while separately identifying exact duplicate sets. No
content is changed until a site administrator approves each proposed action.

## Features

- **Per-page ROT scoring** — every scanned page receives age, traffic, priority,
  similarity, and ROT classification data, including pages that do not belong
  to a duplicate set.
- **Age and staleness reporting** — the ROT report stores creation-based node
  age and last-updated age separately from the normalized 0–1 staleness score
  used for priority, so migration timestamps do not hide legacy content.
- **Similarity evidence without transitive clusters** — semantic search can
  identify near-duplicate pages for per-page ROT review, but related pages are
  not unioned into inferred duplicate groups.
- **Exact duplicate detection** — groups nodes only when their normalized title
  and full editable content match exactly. Shared topics, similar titles, and
  repeated campaign language do not create clusters.
- **Duplicate prioritization** — combines traffic (from uploaded GA4 CSV data)
  and age into a priority score for each exact duplicate set.
- **LLM-generated summaries** — optionally asks an AI model to summarize why
  each cluster is flagged and what action it recommends.
- **Automatic draft edit proposals** — optionally asks an AI model to draft
  updates on canonical pages for merge/refresh clusters and saves them as
  approval-gated revisions.
- **Action queue with approval gate** — proposed actions (prune, merge,
  retire, merge, flag for review, proposed edits) are queued for administrator
  approval before any node is touched.
- **Recommendation workflow** — the ROT Scores view can be sorted by
  recommendation, and each recommendation links to a ROT detail page with a
  review decision or an Action Queue path.
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
- AI Search is not required for exact duplicate detection. It may still be
  enabled elsewhere on the site for search and related-content features.

## Installation

1. Install and enable the AI module if you want AI summaries or agent tools.
2. Install and enable this module.
3. Visit **Admin → Configuration → AI → AI Content Audit** to configure.

## Configuration

Navigate to **Admin → Configuration → AI → AI Content Audit → Settings**.

Key settings:

- **Content scope** — choose which content types are included in the scan.
- **Cron scanning** — enable automatic scans and set the interval and batch
  size.
- **LLM summary model** — model used to generate cluster summaries (can be
  left empty to skip summaries).
- **Automatic draft edits** — optional post-scan AI drafting for canonical
  pages in merge/refresh clusters, with a model choice and per-run cap.
- **Individual AI edit suggestions** — from an eligible page's ROT detail,
  **Suggest AI edit** drafts a revision for that page even when it is not a
  duplicate. The draft remains pending until an administrator reviews the
  diff and approves it from the Action Queue.
- **Traffic data** — upload a GA4 pageviews CSV to factor traffic into duplicate
  prioritization. Nodes with high traffic score lower for pruning.

## Usage

### Running a Scan

Go to **Admin → Configuration → AI → AI Content Audit**. Click **Scan Now**
to start an incremental scan. Progress is shown on the dashboard. Large sites
may require multiple cron runs to complete.

The scan produces a ROT score for every page. It also produces **duplicate
sets** only for exact matches. Similarity evidence contributes to the page's
ROT classification, but semantically related pages are intentionally left out
of the duplicate/consolidation workflow unless their normalized full content
matches exactly.

If automatic draft edits are enabled, the module can also stage draft
revisions on canonical pages for high-value merge/refresh clusters. These are
saved as `edit_proposed` items with `pending` status and still require
administrator approval.

From an individual ROT detail page, **Suggest AI edit** uses the page's
editable source and ROT/lifecycle context to propose a refresh. It does not
merge pages, choose a canonical page, or publish anything; it creates the
same approval-gated `edit_proposed` revision used by the duplicate workflow.

### Reviewing Clusters

Go to **ROT Scores** to review every page from the newest scan. Filter by
Redundant, Outdated, Trivial, or Review. Use the **Recommendation** dropdown
to show only outcomes such as Retire, Refresh, Prune, Merge, or Review; use
**Sort by** separately to order the filtered results. Every row links to a
detail page. Go to
**Duplicates** to see exact duplicate sets ordered by priority. Click a
duplicate set to see:

- All member nodes with their individual scores.
- The selected working canonical page and the exact duplicate candidates.
- The LLM-generated summary (if configured).
- Recommended action for each node.

Historical runs remain available from **Runs**, but the default **Clusters**
view shows only the newest usable scan. A direct link to an older cluster is
marked historical so old labels are not mistaken for current findings.

### Approving Actions

Actions proposed by the scan (or by an AI agent) appear in the **Action Queue**
tab. This includes pending `edit_proposed` draft revisions, which can be
reviewed through their diff links before approval. Select pending items and
click **Approve selected**, then click **Apply approved** when ready. Approved
actions are then executed by the queue processor.

Possible actions per candidate node:

| Action | Effect |
|---|---|
| `prune` | Unpublishes an exact duplicate node. |
| `retire` | Unpublishes a fully stale, very low-traffic legacy page after administrator approval. |
| `merge` | Sends the node into consolidation review; no node is removed automatically. |
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
