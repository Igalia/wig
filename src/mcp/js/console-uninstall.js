(() => {
  const state = window.__wigMcpConsoleState;
  if (state) {
    state.active = false;
    if (Array.isArray(state.buffer))
      state.buffer.length = 0;
    for (const level of ['debug', 'log', 'info', 'warn', 'error']) {
      if (state.wrappers && state.originals && console[level] === state.wrappers[level])
        console[level] = state.originals[level];
    }
    if (state.onError)
      window.removeEventListener('error', state.onError, true);
    if (state.onRejection)
      window.removeEventListener('unhandledrejection', state.onRejection);
  }
  delete window.__wigMcpConsoleState;
  delete window.__wigMcpConsoleMessages;
  delete window.__wigMcpConsoleInstalled;
})();
