/* Read the console buffer installed by console.js.
 *
 * Levels arrive as a comma-separated string because
 * call_async_javascript_function() accepts only numbers, strings, and
 * dictionaries, so an array cannot be passed as an argument. */
(() => {
  const NS = (globalThis.__wig ??= {});
  if (NS.consoleRead)
    return;

  NS.consoleRead = options => {
    options = options || {};
    const buffer = window.__wigMcpConsoleMessages || [];
    const native = options.native ? JSON.parse(options.native) : [];
    const levels = options.levels ? String(options.levels).split(',').filter(Boolean) : null;
    const limit = Math.max(1, Number(options.limit) || 100);

    const wanted = entry => !levels || levels.includes(entry.level);
    const matched = buffer.filter(wanted).concat(native.filter(wanted))
      .sort((first, second) => first.timestamp - second.timestamp);
    const selected = matched.slice(-limit);
    const cleared = [];
    const messages = selected.map(entry => {
      if (entry.id === undefined)
        return entry;
      cleared.push(entry.id);
      const { id, ...message } = entry;
      return message;
    });

    /* Discard only what is being handed over. Draining the whole buffer would
     * throw away entries a level filter or the limit never showed the caller,
     * which is a silent way to lose exactly the messages they were looking
     * for. */
    const clear = Number(options.clear) === 1;
    if (clear && selected.length) {
      const dropped = new Set(selected);
      for (let i = buffer.length - 1; i >= 0; i--) {
        if (dropped.has(buffer[i]))
          buffer.splice(i, 1);
      }
    }

    return { matched: matched.length, returned: messages.length, messages, cleared: clear ? cleared : [] };
  };
})();
