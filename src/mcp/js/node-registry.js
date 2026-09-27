/* Stable node handles for the MCP tools.
 *
 * This lives on the page's own global, as the console instrumentation does. An
 * isolated script world would hide it from page scripts, but an isolated
 * world's global is rebuilt for every evaluation, so nothing stored there
 * survives to the next call. The registry has to outlive a single call, so the
 * default world is the only option.
 *
 * A handle is "<epoch>.<frame>.<n>". The epoch is the committed navigation
 * generation supplied by the UI process, so a handle minted for an earlier
 * document is reported as stale instead of silently resolving to an unrelated
 * element. The frame segment is always 0 for now; it exists so that reaching
 * into subframes later does not change the handle format clients already use.
 *
 * Handles are kept in a WeakMap keyed by element, so a handle stays valid for
 * as long as the element lives. Clients can extract a page once and then
 * interact with it repeatedly rather than re-extracting to refresh handles. */
(() => {
  const NS = (globalThis.__wig ??= {});
  if (NS.registry)
    return;

  const MAIN_FRAME = 0;

  /* Created once per document, keeping the epoch it was born with. The script
   * world's globals are cleared per document, so the registry's lifetime
   * matches the document's without any help.
   *
   * A differing epoch from the caller must not rebuild it. The committed
   * navigation generation advances while a document stays put -- loading a real
   * page over the initial about:blank does exactly that -- and a rebuild would
   * invalidate every handle held for a page that never changed. */
  NS.registry = epoch => {
    let registry = NS.nodeRegistry;
    if (!registry)
      registry = NS.nodeRegistry = { epoch, next: 1, ids: new WeakMap(), refs: new Map() };
    return registry;
  };

  NS.uid = (registry, element) => {
    let n = registry.ids.get(element);
    if (n === undefined) {
      n = registry.next++;
      registry.ids.set(element, n);
      registry.refs.set(n, new WeakRef(element));
    }
    return registry.epoch + '.' + MAIN_FRAME + '.' + n;
  };

  /* Drop handles whose elements have been collected. Called from the walker so
   * the reverse map does not grow without bound on long-lived documents. */
  NS.prune = registry => {
    for (const [n, ref] of registry.refs) {
      if (!ref.deref())
        registry.refs.delete(n);
    }
  };

  /* Resolve a handle to a live element. Returns {element} or {error}, keeping
   * "wrong document", "already collected", and "detached" distinguishable so
   * callers can say which one happened.
   *
   * The epoch is compared against the registry's own, not against whatever the
   * UI process currently believes, so a handle stays valid for as long as its
   * document does. */
  NS.resolve = handle => {
    const registry = NS.nodeRegistry;
    const text = String(handle);
    const parts = text.split('.');
    if (parts.length !== 3)
      return { error: 'Malformed node handle "' + text + '", expected <epoch>.<frame>.<n>' };

    const handleEpoch = Number(parts[0]);
    const frame = Number(parts[1]);
    const n = Number(parts[2]);
    if (!Number.isInteger(handleEpoch) || !Number.isInteger(frame) || !Number.isInteger(n))
      return { error: 'Malformed node handle "' + text + '", expected <epoch>.<frame>.<n>' };
    if (frame !== MAIN_FRAME)
      return { error: 'Node "' + text + '" refers to a subframe, which is not supported yet' };
    if (!registry || handleEpoch !== registry.epoch) {
      return { error: 'Node "' + text + '" is stale: the page navigated since it was issued. '
          + 'Extract the page again.' };
    }

    const element = registry.refs.get(n)?.deref();
    if (!element)
      return { error: 'Unknown node "' + text + '". Extract the page again to get current handles.' };
    if (!element.isConnected)
      return { error: 'Node "' + text + '" is no longer in the document' };

    return { element };
  };
})();
