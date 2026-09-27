(() => {
  if (window.__wigMcpConsoleInstalled)
    return;
  window.__wigMcpConsoleInstalled = true;

  const buffer = window.__wigMcpConsoleMessages = [];
  const state = window.__wigMcpConsoleState = {
    active: true,
    buffer,
    originals: {},
    wrappers: {},
  };

  const record = entry => {
    try {
      if (!state.active)
        return;
      buffer.push(entry);
      if (buffer.length > 1000)
        buffer.splice(0, buffer.length - 1000);
    } catch (e) {
      // Never let logging instrumentation break the page.
    }
  };

  /* An uncaught exception or a rejected promise nobody handled is usually the
   * first thing worth knowing about a broken page, and neither passes through
   * console.*, so wrapping those five methods alone would miss both. */
  state.onError = event => record({
    level: 'error',
    source: 'exception',
    timestamp: Date.now(),
    text: String(event.message || 'Uncaught error'),
    args: [],
    stack: event.error && event.error.stack ? String(event.error.stack) : '',
    url: String(event.filename || ''),
    line: Number(event.lineno || 0),
  });

  state.onRejection = event => {
    const reason = event.reason;
    record({
      level: 'error',
      source: 'rejection',
      timestamp: Date.now(),
      text: reason && reason.message ? String(reason.message) : String(reason),
      args: [],
      stack: reason && reason.stack ? String(reason.stack) : '',
    });
  };

  window.addEventListener('error', state.onError, true);
  window.addEventListener('unhandledrejection', state.onRejection);

  for (const level of ['debug', 'log', 'info', 'warn', 'error']) {
    const original = console[level];
    state.originals[level] = original;
    const wrapper = function (...args) {
      try {
        if (state.active) {
          const structured = args.map(value => {
            try {
              return JSON.parse(JSON.stringify(value));
            } catch (e) {
              return String(value);
            }
          });
          const text = args.map(value => {
            try {
              return typeof value === 'string' ? value : JSON.stringify(value);
            } catch (e) {
              return String(value);
            }
          }).join(' ');
          record({
            level,
            source: 'console',
            timestamp: Date.now(),
            text,
            args: structured,
            stack: (new Error()).stack,
          });
        }
      } catch (e) {
        // Never let logging instrumentation break the page.
      }
      return original.apply(this, args);
    };
    state.wrappers[level] = wrapper;
    console[level] = wrapper;
  }
})();
