/* Run a batch of page interactions.
 *
 * Targets are resolved in the order node, selector, text: a uid handed out by
 * the textTree extractor, a CSS selector, or the visible text of a control.
 * Handle targeting is preferred because it says exactly which element was meant
 * and reports precisely why it failed, where a selector can silently match a
 * different element than the caller had in mind.
 *
 * Actions run in order and stop at the first failure, so a later step can rely
 * on an earlier one having happened. */
(() => {
  const NS = (globalThis.__wig ??= {});
  if (NS.interact)
    return;

  const TARGETABLE = 'a[href],button,input,select,textarea,summary,label,[role],[tabindex],[onclick]';

  const collapse = text => String(text).replace(/\s+/g, ' ').trim();

  const isVisible = element => {
    if (typeof element.checkVisibility === 'function'
        && !element.checkVisibility({ opacityProperty: true, visibilityProperty: true, contentVisibilityAuto: true }))
      return false;
    const rect = element.getBoundingClientRect();
    return rect.width > 0 || rect.height > 0;
  };

  /* querySelectorAll does not cross a shadow boundary, so descend into every
   * open root. Visiting a host's shadow content directly after the host keeps
   * the order close to what the page renders. */
  function collectTargets(root, found) {
    for (const element of root.querySelectorAll('*')) {
      if (element.matches(TARGETABLE))
        found.push(element);
      if (element.shadowRoot)
        collectTargets(element.shadowRoot, found);
    }
    return found;
  }

  function shadowRootMatches(selector) {
    for (const host of document.querySelectorAll('*')) {
      if (host.shadowRoot && host.shadowRoot.querySelector(selector))
        return true;
    }
    return false;
  }

  /* Prefer an exact match on visible text or accessible name, falling back to
   * the first containing match so that "Send" finds "Send It". */
  function findByText(text) {
    const wanted = collapse(text).toLowerCase();
    if (!wanted)
      return { error: 'text must not be empty' };

    let partial = null;
    for (const element of collectTargets(document, [])) {
      if (!isVisible(element))
        continue;
      const own = collapse(element.textContent).toLowerCase();
      const label = collapse(element.getAttribute('aria-label') || '').toLowerCase();
      if (own === wanted || label === wanted)
        return { element };
      if (!partial && (own.includes(wanted) || label.includes(wanted)))
        partial = element;
    }

    return partial ? { element: partial } : { error: 'No visible element matches text "' + text + '"' };
  }

  function resolveTarget(action) {
    if (action.node !== undefined && action.node !== null)
      return NS.resolve(action.node);
    if (action.selector) {
      const element = document.querySelector(action.selector);
      if (element)
        return { element };
      if (shadowRootMatches(action.selector)) {
        return { error: 'Selector "' + action.selector + '" only matches inside a shadow root, which CSS cannot '
            + 'reach. Use the node handle that get_page_content reports for it.' };
      }
      return { error: 'Selector not found: ' + action.selector };
    }
    if (action.text)
      return findByText(action.text);
    return { error: 'Action needs one of node, selector, or text' };
  }

  NS.interact = (actions, options) => {
    const outcomes = [];
    let failure = null;

    for (let i = 0; i < actions.length && !failure; i++) {
      const action = actions[i];
      try {
        /* type and selectOption need somewhere to put their value; scroll and
         * keyPress work on the window or the focused element unless a target
         * was named. */
        const optionalTarget = action.type === 'scroll' || action.type === 'keyPress';
        const named = action.node !== undefined || action.selector !== undefined || action.text !== undefined;

        let element = null;
        if (!optionalTarget || named) {
          const resolved = resolveTarget(action);
          if (resolved.error)
            throw new Error(resolved.error);
          element = resolved.element;
        }

        if (element && action.scrollToVisible)
          element.scrollIntoView({ block: 'center', inline: 'center' });

        switch (action.type) {
        case 'click':
          element.click();
          break;
        case 'type': {
          if (action.value === undefined) {
            throw new Error('type needs the text to enter in "value"; "text" selects the target element');
          }
          const value = String(action.value);
          element.focus();
          element.value = value;
          element.dispatchEvent(new InputEvent('input', { bubbles: true, inputType: 'insertText', data: value }));
          element.dispatchEvent(new Event('change', { bubbles: true }));
          break;
        }
        case 'focus':
          element.focus();
          break;
        case 'scroll':
          if (element)
            element.scrollIntoView({ block: 'center', inline: 'center' });
          else
            window.scrollBy(Number(action.x || 0), Number(action.y || 0));
          break;
        case 'hover':
          for (const name of ['mouseover', 'mouseenter', 'mousemove'])
            element.dispatchEvent(new MouseEvent(name, { bubbles: true }));
          break;
        case 'keyPress': {
          const focus = element || document.activeElement || document.body;
          const key = String(action.key || action.value || '');
          if (!key)
            throw new Error('keyPress needs a key');
          for (const name of ['keydown', 'keypress', 'keyup'])
            focus.dispatchEvent(new KeyboardEvent(name, { key, code: key, bubbles: true }));
          break;
        }
        case 'selectOption': {
          const value = String(action.value !== undefined ? action.value : '');
          element.value = value;
          element.dispatchEvent(new Event('input', { bubbles: true }));
          element.dispatchEvent(new Event('change', { bubbles: true }));
          if (element.value !== value)
            throw new Error('Option not found: ' + value);
          break;
        }
        default:
          throw new Error('Unsupported action type: ' + action.type);
        }

        outcomes.push({ index: i, ok: true, type: action.type });
      } catch (error) {
        const message = String(error && error.message ? error.message : error);
        outcomes.push({ index: i, ok: false, type: action.type, error: message });
        failure = 'Action ' + i + ' failed: ' + message;
      }
    }

    const result = failure ? { ok: false, outcomes, error: failure } : { ok: true, outcomes };

    /* Report the resulting page in the same call, so observing an effect does
     * not need a second round trip. Included on failure too: what the page
     * looks like after a partial batch is exactly what a caller needs to
     * recover. */
    if ((options || {}).returnContent === 'textTree' && NS.walk && NS.text)
      result.content = NS.text(NS.walk(options), options);

    return result;
  };
})();
