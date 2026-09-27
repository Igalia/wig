# Wig MCP server

Wig can be driven by a [Model Context Protocol](https://modelcontextprotocol.io)
client. The server exposes the browser as a set of tools for managing tabs,
navigating, reading and interacting with pages, taking screenshots, and
inspecting console output, network traffic, and script dialogs.

Running `wig --mcp-stdio` speaks MCP over standard input and output. This is the
only transport, and it is intended for clients that manage local MCP processes.
GApplication forwards the invoking process's stdin and stdout to the primary
Wig instance, so the tools act on the browser windows that are already open. If
Wig is not running yet, the command starts it.

While a client is connected:

- script dialogs (`alert`, `confirm`, `prompt`, `beforeunload`) are held for the
  client to answer through `browser_dialogs` instead of being shown in the tab;
- navigations the client causes are not recorded in history;
- console and network instrumentation is installed in every tab, and removed
  again once no session is active.

Connected sessions are listed/managed in the MCP Server pane of the settings
page, `wig:settings/mcp`.
Internal `wig:` pages themselves cannot be loaded or inspected
by a client.

Protocol parsing, lifecycle enforcement, sessions, and stdio framing are
provided by the top-level `mcp-glib` static library. This directory supplies the
stdio connections and the WebKit tools.

## Conventions

**Tabs.** Tabs are identified by an integer `tab_handle` and windows by a
`window_handle`, both stable for the life of the process. Every tool that acts
on a tab takes an optional `tab_handle`; without one it uses the selected tab of
the active window, opening a window or a blank tab first if there is none.

**Results.** Unless a tool says otherwise, a successful call returns a single
text block holding JSON. A failed call returns a text block with the error
message and `isError` set. Arguments a tool does not accept are rejected rather
than ignored.

**Page info.** Several tools return this object:

| Field        | Type           | Description                          |
| ------------ | -------------- | ------------------------------------ |
| `tab_handle` | integer        | The tab.                             |
| `url`        | string or null | Its current URL.                     |
| `title`      | string or null | Its current title.                   |
| `loading`    | boolean        | Whether a load is in progress.       |
| `progress`   | number         | Estimated load progress, 0.0 to 1.0. |

**Limits.** Results are bounded. Free text is truncated at 256 kB with a marker
saying how much was dropped. A result that has to stay parseable, such as JSON
from a script, is reported as an error instead of being cut, and so is a
screenshot over 4 MiB.

## Tabs

### `list_tabs`

Lists every tab in every window.

Takes no arguments.

Returns an array with one object per tab:

| Field           | Type           | Description                                   |
| --------------- | -------------- | --------------------------------------------- |
| `tab_handle`    | integer        | The tab.                                      |
| `window_handle` | integer        | The window holding it.                        |
| `url`           | string or null | Its current URL.                              |
| `title`         | string or null | Its current title.                            |
| `active`        | boolean        | Whether it is the selected tab of its window. |

`active` is scoped to a window, so with several windows open several tabs are
active and `window_handle` tells them apart.

### `create_tab`

Opens a new tab in the active window, selects it, and starts loading a URL.

| Argument | Type   | Default       | Description      |
| -------- | ------ | ------------- | ---------------- |
| `url`    | string | `about:blank` | The URL to load. |

Returns the new tab's page info. The load has only started; use
`wait_for_navigation` to wait for it.

### `switch_tab`

Selects a tab and presents its window.

| Argument     | Type    | Default  | Description        |
| ------------ | ------- | -------- | ------------------ |
| `tab_handle` | integer | required | The tab to select. |

Returns the tab's page info.

### `close_tab`

Closes a tab.

| Argument     | Type    | Default  | Description       |
| ------------ | ------- | -------- | ----------------- |
| `tab_handle` | integer | required | The tab to close. |

Returns `{"closed": true, "tab_handle": <handle>}`.

### `page_info`

Reports the state of a tab.

| Argument     | Type    | Default    | Description |
| ------------ | ------- | ---------- | ----------- |
| `tab_handle` | integer | active tab | The tab.    |

Returns the tab's page info.

## Navigation

### `navigate_to_url`

Loads a URL and waits for the navigation to finish.

| Argument     | Type    | Default    | Description                            |
| ------------ | ------- | ---------- | -------------------------------------- |
| `url`        | string  | required   | The URL to load.                       |
| `tab_handle` | integer | active tab | The tab.                               |
| `timeout`    | number  | 30         | Seconds to wait, from 0.1 through 30. |

Returns the page info with an extra `text` field holding the page's rendered
text. Only the navigation that was requested counts, so an earlier load
finishing does not complete the call. Timing out, the load failing, a newer load
superseding it, or the tab closing is reported as an error.

### `wait_for_navigation`

Waits for the tab's current navigation to finish, or returns straight away if
nothing is loading.

| Argument     | Type    | Default    | Description                            |
| ------------ | ------- | ---------- | -------------------------------------- |
| `tab_handle` | integer | active tab | The tab.                               |
| `timeout`    | number  | 30         | Seconds to wait, from 0.1 through 30. |

Returns the tab's page info.

## Page content

### `get_page_content`

Extracts the content of the page.

| Argument                  | Type    | Default    | Description                                                    |
| ------------------------- | ------- | ---------- | -------------------------------------------------------------- |
| `tab_handle`              | integer | active tab | The tab.                                                       |
| `format`                  | string  | `text`     | One of `textTree`, `text`, `html`, `markdown`, or `json`.      |
| `region`                  | string  | `viewport` | `textTree` only: `viewport` or `document`.                     |
| `max_nodes`               | integer | 1500       | `textTree` only: node limit, from 1 through 20000.             |
| `max_words_per_paragraph` | integer | 30         | `textTree` only: words kept per paragraph, 1 through 2000.     |
| `include_containers`      | boolean | false      | `textTree` only: keep structural containers with no role.      |

Returns plain text rather than JSON, except for the `json` format:

- `text` is the page's rendered text.
- `html` is the serialized document.
- `markdown` keeps headings, links, emphasis, and lists, and leaves out hidden
  and unrendered content.
- `json` is `{"url", "title", "text", "links": [{"text", "href"}]}`.
- `textTree` is an indented outline of the rendered page, described below.

The `textTree` output starts with a header line giving the scroll position, the
content and viewport sizes, and the region:

```
root scroll=(0,0) content=[1280×2400] viewport=[1280×720] region=viewport
```

Each following line is one node, indented by tabs to its depth: its kind, then
attributes such as `uid=`, `level=`, `label=`, `url=`, `value=`, `checked`,
`expanded`, `disabled`, `required`, and `focused`, then its text in quotes.
Hidden content is omitted and prose is cut to `max_words_per_paragraph` words.
When extraction stops at `max_nodes` nodes or 256 kB, the header says so with
`truncated=`.

Every interactive element carries a `uid=<epoch>.<frame>.<n>` handle that
`page_interactions` can target. A handle stays valid until its element leaves
the document, so a page does not have to be extracted again before acting on
it. Handles from a previous document are reported as stale rather than
resolving to something unrelated.

Extraction follows the flattened tree: an open shadow root is walked in place of
its host's own children, and a `<slot>` contributes whatever was assigned to it,
each slotted node appearing once, where it renders. A closed shadow root is
invisible to page script, so it is left out entirely.

The extractor and its node registry run in the page's own script world, under a
`__wig` global, because an isolated world's global object is rebuilt on every
evaluation and the registry has to outlive a single call.

### `evaluate_javascript`

Evaluates a script in the page.

| Argument     | Type    | Default    | Description             |
| ------------ | ------- | ---------- | ----------------------- |
| `script`     | string  | required   | The script to evaluate. |
| `tab_handle` | integer | active tab | The tab.                |

Returns the script's completion value serialized as JSON. A thrown exception is
reported as an error, and so is a result over 256 kB. A navigation the script
starts is not recorded in history.

## Interaction

### `page_interactions`

Runs a sequence of actions in the page, stopping at the first one that fails.

| Argument         | Type    | Default    | Description                                                   |
| ---------------- | ------- | ---------- | ------------------------------------------------------------- |
| `actions`        | array   | required   | The actions to run, described below.                          |
| `tab_handle`     | integer | active tab | The tab.                                                      |
| `return_content` | string  | `textTree` | `textTree` to return the resulting page, or `none`.           |
| `region`         | string  | `viewport` | Region of the returned page: `viewport` or `document`.        |

Each action is an object with a `type` and the fields that type needs:

| Field             | Type    | Description                                                        |
| ----------------- | ------- | ------------------------------------------------------------------ |
| `type`            | string  | `click`, `type`, `focus`, `scroll`, `hover`, `keyPress`, or `selectOption`. |
| `node`            | string  | Target by a `uid` handle from a `textTree`.                        |
| `selector`        | string  | Target by CSS selector.                                            |
| `text`            | string  | Target by visible text or accessible name.                         |
| `value`           | any     | The text to enter for `type`, or the option value for `selectOption`. |
| `key`             | string  | The key for `keyPress`.                                            |
| `scrollToVisible` | boolean | Scroll the target into view before acting.                         |
| `x`, `y`          | number  | Scroll offset for `scroll` without a target.                       |

Every action other than `scroll` and `keyPress` needs a target, given as one of
`node`, `selector`, or `text`. Without one, `scroll` scrolls the window and
`keyPress` goes to the focused element. `node` is preferred, as it reports
exactly why a target could not be used. `node` and `text` reach into open shadow
roots, but `selector` cannot, since CSS has no piercing combinator; a selector
that matches only inside a shadow root says so rather than reporting an
ordinary miss.

Returns two text blocks. The first is JSON:

| Field      | Type    | Description                                                        |
| ---------- | ------- | ------------------------------------------------------------------ |
| `ok`       | boolean | Whether every action succeeded.                                    |
| `outcomes` | array   | One `{"index", "ok", "type", "error"}` per action that was run.    |
| `error`    | string  | Present on failure: which action failed and why.                   |

The second is the resulting page as a `textTree`, held to 64 kB. It is returned
on failure too, since that is when a caller most needs to see where the page
ended up, and it is left out when `return_content` is `none`. A failed action
sets `isError`. A navigation the actions start is not recorded in history.

## Screenshots and viewport

### `screenshot`

Captures the page as a PNG.

| Argument        | Type    | Default    | Description                                         |
| --------------- | ------- | ---------- | --------------------------------------------------- |
| `tab_handle`    | integer | active tab | The tab.                                            |
| `full_document` | boolean | false      | Capture the whole document instead of the viewport. |

Returns an image block with `mimeType` `image/png`. A capture over 4 MiB is
reported as an error that gives its size.

### `set_viewport_size`

Resizes the tab's window so that its page area approximates a CSS viewport
size.

| Argument     | Type    | Default    | Description                        |
| ------------ | ------- | ---------- | ---------------------------------- |
| `width`      | integer | required   | Width in pixels, 1 through 16384.  |
| `height`     | integer | required   | Height in pixels, 1 through 16384. |
| `tab_handle` | integer | active tab | The tab.                           |

Returns:

| Field                       | Type    | Description                                                  |
| --------------------------- | ------- | ------------------------------------------------------------ |
| `requested_width`           | integer | The width asked for.                                         |
| `requested_height`          | integer | The height asked for.                                        |
| `platform_resize_requested` | boolean | Whether the platform accepted a resize of the toplevel.      |
| `best_effort`               | boolean | True when only the window's default size could be set instead. |

## Console

### `browser_console_messages`

Returns buffered console messages for a tab, oldest first.

| Argument       | Type    | Default    | Description                                                        |
| -------------- | ------- | ---------- | ------------------------------------------------------------------ |
| `tab_handle`   | integer | active tab | The tab.                                                           |
| `level_filter` | array   | all levels | Levels to include: `debug`, `log`, `info`, `warn`, `error`.        |
| `limit`        | integer | 100        | The most recent matching messages to return, 1 through 1000.       |
| `clear`        | boolean | false      | Remove the returned messages from the buffer.                      |

Returns:

| Field      | Type    | Description                                     |
| ---------- | ------- | ----------------------------------------------- |
| `matched`  | integer | Messages matching the filter.                   |
| `returned` | integer | Messages returned, at most `limit`.             |
| `messages` | array   | The messages.                                   |

Each message has a `level`, a `source`, a `timestamp` in milliseconds since the
epoch, and its `text`. The `source` says where it came from:

| Source      | Origin                                                  | Extra fields           |
| ----------- | ------------------------------------------------------- | ---------------------- |
| `console`   | A call to `console.*`.                                  | `args`, `stack`        |
| `exception` | An uncaught error.                                      | `stack`, `url`, `line` |
| `rejection` | An unhandled promise rejection.                         | `stack`                |
| `network`   | The browser, such as a failed load.                     | `url`, `line`          |
| `security`  | The browser, such as a CSP violation or mixed content.  | `url`, `line`          |
| `other`     | The browser, anything else.                             | `url`, `line`          |

The first three are captured in the page, which keeps the newest 1000 of them
for the current document. The rest are produced by the browser itself, where
page script cannot see them, and are forwarded by a web process extension. The
newest 500 of those are kept across navigations, like network requests. Both
are merged in timestamp order before the filter and limit apply.

`clear` discards only what was returned, so a filter or limit never silently
drops messages the caller has not seen.

## Network

### `list_network_requests`

Lists summaries of a tab's network requests, newest last.

| Argument     | Type    | Default    | Description                                                     |
| ------------ | ------- | ---------- | --------------------------------------------------------------- |
| `tab_handle` | integer | active tab | The tab.                                                        |
| `filter`     | object  | none       | Filters, described below.                                       |
| `since`      | number  | none       | Only requests that started at or after this `start` value.      |
| `limit`      | integer | 500        | The most recent matching requests to return, 1 through 500.     |
| `clear`      | boolean | false      | Remove the returned requests from the buffer.                   |

The `filter` object accepts `url_substring` and `method`, both matched without
regard to case, and a `status_min`/`status_max` range. A request that has not
had a response yet has no status, so it never matches a status range.

Returns:

| Field      | Type    | Description                           |
| ---------- | ------- | ------------------------------------- |
| `matched`  | integer | Requests matching the filter.         |
| `returned` | integer | Requests returned, at most `limit`.   |
| `requests` | array   | The request summaries.                |

Each summary has:

| Field        | Type            | Description                                           |
| ------------ | --------------- | ----------------------------------------------------- |
| `request_id` | integer         | Pass to `get_network_request` for the full details.   |
| `navigation` | integer         | The navigation that issued it.                        |
| `url`        | string or null  | The request URL.                                      |
| `method`     | string or null  | The HTTP method.                                      |
| `status`     | integer or null | The response status, once there is one.               |
| `mime_type`  | string or null  | The response MIME type.                               |
| `start`      | number          | When it started, in milliseconds since the epoch.     |
| `end`        | number or null  | When it finished.                                     |
| `duration`   | number or null  | How long it took, in milliseconds.                    |
| `error`      | string or null  | Why it failed, if it did.                             |

The buffer survives navigations and holds the newest 500 requests. `clear`
discards only what was returned.

### `get_network_request`

Returns the full details of one captured request, including its response body.

| Argument     | Type    | Default    | Description                                          |
| ------------ | ------- | ---------- | ---------------------------------------------------- |
| `request_id` | integer | required   | A `request_id` from `list_network_requests`.         |
| `tab_handle` | integer | active tab | The tab the request belongs to.                      |

Returns:

| Field                     | Type    | Description                                                  |
| ------------------------- | ------- | ------------------------------------------------------------ |
| `request`                 | object  | The request summary, as in `list_network_requests`.          |
| `request_headers`         | object  | Request header names mapped to values.                       |
| `response_headers`        | object  | Response header names mapped to values.                      |
| `request_body`            | null    | Always null: WebKit does not expose upload bodies.           |
| `request_body_available`  | boolean | Always false.                                                |
| `request_body_note`       | string  | Says why the request body is missing.                        |
| `response_body`           | string  | The body, as text or base64.                                 |
| `encoding`                | string  | `text` for textual MIME types, otherwise `base64`.           |
| `response_body_bytes`     | integer | The body's full size.                                        |
| `response_body_truncated` | boolean | Whether the body was cut at 10 MiB.                          |

## Dialogs

### `browser_dialogs`

Lists the script dialogs waiting on a tab, or answers one.

| Argument     | Type    | Default    | Description                                     |
| ------------ | ------- | ---------- | ----------------------------------------------- |
| `tab_handle` | integer | active tab | The tab.                                        |
| `dialog_id`  | integer | none       | The dialog to answer.                           |
| `action`     | string  | none       | `accept` or `dismiss`.                          |
| `text`       | string  | none       | The reply to a `prompt` when accepting it.      |

Without `dialog_id` or `action`, returns an array of the pending dialogs, each
with a `dialog_id`, a `type` of `alert`, `confirm`, `prompt`, or `beforeunload`,
its `message`, and for a prompt its `default_text`.

A dialog pauses the page until it is answered, so a call that is waiting on the
page when one opens (`page_interactions`, `evaluate_javascript`,
`get_page_content`, `navigate_to_url`, `wait_for_navigation`, and so on) returns
at once with a message naming the dialog and its `dialog_id`, rather than
waiting for an answer that cannot arrive while the call is outstanding.

With both, answers the dialog and returns `{"responded": true}`. Dialogs are
only held while a session has finished initializing; any still pending when the
last one ends are closed.
