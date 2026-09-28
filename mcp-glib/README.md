# mcp-glib

This library currently targets MCP protocol version `2025-11-25` and owns:

- strict JSON-RPC parsing, validation, and errors;
- MCP initialization, version negotiation, and session state;
- built-in ping and cancellation handling;
- registered synchronous and asynchronous server methods;
- newline-delimited stdio connections supplied as `GInputStream` instances;
- a custom transport exchange API for embedding other transports.

Public APIs are grouped by responsibility in `mcp-server.h`, `mcp-session.h`,
`mcp-call.h`, and `mcp-transport.h`. `mcp-glib.h` is only an umbrella header.

Normative references:

- [Base protocol](https://modelcontextprotocol.io/specification/2025-11-25/basic)
- [Lifecycle](https://modelcontextprotocol.io/specification/2025-11-25/basic/lifecycle)
- [Transports](https://modelcontextprotocol.io/specification/2025-11-25/basic/transports)
- [JSON-RPC 2.0](https://www.jsonrpc.org/specification)
