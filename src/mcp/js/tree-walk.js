/* Build a compact, addressable view of the rendered page.
 *
 * A flat list carrying an explicit depth is cheaper to produce than a nested
 * structure, and the text renderer turns depth back into indentation. Skipping
 * unrendered content, truncating prose, and giving interactive elements a
 * stable handle from node-registry.js together keep the result small enough to
 * hand to a model while leaving it something that can be acted on.
 *
 * Only the node count is bounded here; the byte ceiling belongs to the
 * renderer, the first place the output's real size is known. */
(() => {
  const NS = (globalThis.__wig ??= {});
  if (NS.walk)
    return;

  const SKIP_TAGS = new Set(['SCRIPT', 'STYLE', 'NOSCRIPT', 'TEMPLATE', 'HEAD', 'META', 'LINK', 'TITLE', 'BASE',
                             'OPTION', 'OPTGROUP']);

  /* Non-interactive inline elements do not become nodes; their text folds into
   * the surrounding run so a sentence is not split at every <span>. SLOT is
   * here because it is a transparent insertion point: what it contributes is
   * whatever was assigned to it. */
  const INLINE_TAGS = new Set(['SPAN', 'STRONG', 'B', 'EM', 'I', 'CODE', 'SMALL', 'SUB', 'SUP', 'MARK', 'U', 'S',
                               'ABBR', 'CITE', 'Q', 'TIME', 'KBD', 'SAMP', 'VAR', 'DEL', 'INS', 'RUBY', 'RT', 'RP',
                               'BDI', 'BDO', 'DFN', 'FONT', 'BIG', 'TT', 'WBR', 'NOBR', 'SLOT']);

  const CONTAINER_TAGS = new Set(['MAIN', 'NAV', 'HEADER', 'FOOTER', 'ASIDE', 'SECTION', 'ARTICLE', 'TABLE',
                                  'THEAD', 'TBODY', 'TR', 'UL', 'OL', 'DL', 'FIGURE', 'FIELDSET', 'DIALOG',
                                  'DETAILS', 'BLOCKQUOTE']);

  const INTERACTIVE_ROLES = new Set(['button', 'link', 'checkbox', 'radio', 'menuitem', 'menuitemcheckbox',
                                     'menuitemradio', 'option', 'switch', 'tab', 'textbox', 'combobox',
                                     'searchbox', 'slider', 'spinbutton', 'treeitem']);

  const HEADING_LEVELS = { H1: 1, H2: 2, H3: 3, H4: 4, H5: 5, H6: 6 };

  const INTERACTIVE_SELECTOR = 'a[href],button,input,select,textarea,summary,[tabindex],[contenteditable],[role]';

  const collapse = text => String(text).replace(/\s+/g, ' ').trim();

  const truncateWords = (text, maxWords) => {
    const words = String(text).split(/\s+/).filter(Boolean);
    if (words.length <= maxWords)
      return words.join(' ');
    return words.slice(0, maxWords).join(' ') + '…';
  };

  const isEditable = element => {
    const value = element.getAttribute('contenteditable');
    return value !== null && value !== 'false';
  };

  function isInteractive(element) {
    switch (element.tagName) {
    case 'A':
      return element.hasAttribute('href');
    case 'BUTTON':
    case 'SELECT':
    case 'TEXTAREA':
    case 'SUMMARY':
      return true;
    case 'INPUT':
      return (element.type || '').toLowerCase() !== 'hidden';
    }

    if (isEditable(element))
      return true;

    const tabindex = element.getAttribute('tabindex');
    if (tabindex !== null && tabindex.trim() !== '-1')
      return true;

    const role = element.getAttribute('role');
    if (role && INTERACTIVE_ROLES.has(role.trim().toLowerCase()))
      return true;

    return element.hasAttribute('onclick');
  }

  function kindOf(element) {
    const tag = element.tagName;

    /* An explicit interactive role describes the element better than its tag
     * does: role="button" on a div is a button as far as a client cares. */
    const role = element.getAttribute('role');
    if (role) {
      const normalized = role.trim().toLowerCase();
      if (INTERACTIVE_ROLES.has(normalized))
        return normalized;
    }

    switch (tag) {
    case 'A': return 'link';
    case 'BUTTON': return 'button';
    case 'SELECT': return 'select';
    case 'TEXTAREA': return 'textarea';
    case 'SUMMARY': return 'summary';
    case 'IMG': return 'image';
    case 'IFRAME':
    case 'FRAME': return 'iframe';
    case 'FORM': return 'form';
    case 'INPUT': {
      const type = (element.type || 'text').toLowerCase();
      return type === 'checkbox' || type === 'radio' ? type : 'input';
    }
    }
    if (HEADING_LEVELS[tag])
      return 'heading';
    return tag.toLowerCase();
  }

  /* An approximation of the accessible name, good enough to label a control
   * without implementing the full ARIA computation. */
  function accessibleName(element) {
    const label = element.getAttribute('aria-label');
    if (label && label.trim())
      return collapse(label);

    const labelledBy = element.getAttribute('aria-labelledby');
    if (labelledBy) {
      const text = labelledBy.split(/\s+/)
        .map(id => element.ownerDocument.getElementById(id))
        .filter(Boolean)
        .map(node => collapse(node.textContent))
        .filter(Boolean)
        .join(' ');
      if (text)
        return text;
    }

    if (element.labels && element.labels.length) {
      const text = Array.from(element.labels).map(node => collapse(node.textContent)).filter(Boolean).join(' ');
      if (text)
        return text;
    }

    for (const attribute of ['alt', 'title', 'placeholder']) {
      const value = element.getAttribute(attribute);
      if (value && value.trim())
        return collapse(value);
    }

    return '';
  }

  function isRendered(element) {
    if (typeof element.checkVisibility !== 'function')
      return true;
    if (element.checkVisibility({ opacityProperty: true, visibilityProperty: true, contentVisibilityAuto: true }))
      return true;

    /* checkVisibility() reports an element with no box as invisible, but a
     * display: contents wrapper has none and its children are rendered, so
     * treating it as hidden loses the whole subtree. */
    return getComputedStyle(element).display === 'contents';
  }

  /* The flattened tree, which is what the page actually renders: a shadow host
   * shows its shadow content in place of its own children, and a slot inside
   * that content shows the light DOM assigned to it. Walking a host's own
   * children as well would emit everything slotted twice.
   *
   * Only open roots are reachable. A closed root is invisible to page script,
   * so its content cannot be extracted or addressed at all. */
  function childrenOf(element) {
    if (element.shadowRoot)
      return element.shadowRoot.childNodes;

    if (element.tagName === 'SLOT' && typeof element.assignedNodes === 'function') {
      const assigned = element.assignedNodes({ flatten: true });
      if (assigned.length)
        return assigned;
    }

    return element.childNodes;
  }

  NS.walk = options => {
    options = options || {};

    const epoch = Number(options.epoch) || 0;
    const wholeDocument = options.region === 'document';
    const maxNodes = Math.max(1, Number(options.maxNodes) || 1500);
    const maxWords = Math.max(1, Number(options.maxWords) || 30);
    const includeContainers = Number(options.includeContainers) === 1;

    const registry = NS.registry(epoch);
    NS.prune(registry);

    const origin = location.origin;
    const viewWidth = window.innerWidth;
    const viewHeight = window.innerHeight;

    const nodes = [];
    let truncated = '';

    /* The pending text run, plus the depth and rect it inherits from the block
     * that started it. Recorded at first contribution so nested inline
     * recursion does not have to save and restore them. */
    let run = [];
    let runDepth = 0;
    let runRect = null;

    const inRegion = rect => {
      if (wholeDocument)
        return true;
      return rect.bottom > 0 && rect.top < viewHeight && rect.right > 0 && rect.left < viewWidth;
    };

    function shorten(url) {
      if (!url)
        return url;
      let text = String(url);
      if (origin && origin !== 'null' && text.startsWith(origin))
        text = text.slice(origin.length) || '/';
      return text.length > 200 ? text.slice(0, 200) + '…' : text;
    }

    /* Each node's text is already capped at maxWords, so a node count is enough
     * to bound the array. The byte ceiling belongs to the renderer, the only
     * place the output's real size is known. */
    function push(node) {
      if (truncated)
        return false;
      if (nodes.length >= maxNodes) {
        truncated = 'nodes';
        return false;
      }

      nodes.push(node);
      return true;
    }

    function addText(text, depth, rect) {
      if (!run.length) {
        runDepth = depth;
        runRect = rect;
      }
      run.push(text);
    }

    function flushRun() {
      if (!run.length)
        return;
      const text = truncateWords(run.join(''), maxWords);
      const rect = runRect;
      run = [];
      runRect = null;
      if (text && (!rect || inRegion(rect)))
        push({ depth: runDepth, kind: 'text', text });
    }

    /* `withText` is false for elements the walker descends into: their
     * descendants become nodes of their own, so folding textContent in here as
     * well would repeat the whole subtree on one line. */
    function describe(element, depth, withUid, withText) {
      const tag = element.tagName;
      const node = { depth, kind: kindOf(element) };
      if (withUid)
        node.uid = NS.uid(registry, element);

      const level = HEADING_LEVELS[tag];
      if (level)
        node.level = level;

      const name = accessibleName(element);

      if (tag === 'INPUT' || tag === 'TEXTAREA') {
        if (name)
          node.label = name;
        const type = (element.type || 'text').toLowerCase();
        if (type === 'checkbox' || type === 'radio')
          node.checked = element.checked ? 1 : 0;
        else if (element.value)
          node.value = truncateWords(element.value, maxWords);
      } else if (tag === 'SELECT') {
        if (name)
          node.label = name;
        const selected = element.selectedOptions && element.selectedOptions[0];
        if (selected)
          node.value = collapse(selected.textContent);
      } else if (tag === 'IMG') {
        if (name)
          node.text = name;
      } else {
        const text = withText ? truncateWords(collapse(element.textContent), maxWords) : '';
        if (text)
          node.text = text;
        /* Keep the accessible name when it is not simply a restatement of the
         * visible text, so an aria-label on a glyph-only control survives. */
        if (name && collapse(name).toLowerCase() !== collapse(text).toLowerCase())
          node.label = name;
      }

      if (tag === 'A')
        node.url = shorten(element.href);
      else if (tag === 'IMG' || tag === 'IFRAME' || tag === 'FRAME') {
        if (element.getAttribute('src'))
          node.url = shorten(element.src);
      }

      if (element.disabled)
        node.disabled = 1;
      if (element.required)
        node.required = 1;
      if (element === element.ownerDocument.activeElement)
        node.focused = 1;

      const expanded = element.getAttribute('aria-expanded');
      if (expanded === 'true' || expanded === 'false')
        node.expanded = expanded === 'true' ? 1 : 0;

      return node;
    }

    function visitChildren(element, depth, rect, skipText) {
      for (const child of childrenOf(element)) {
        if (child.nodeType === Node.TEXT_NODE) {
          if (!skipText)
            addText(child.data, depth, rect);
        } else if (child.nodeType === Node.ELEMENT_NODE) {
          visit(child, depth, rect);
        }
        if (truncated)
          return;
      }
    }

    function visit(element, depth, inheritedRect) {
      if (truncated)
        return;

      const tag = element.tagName;
      if (SKIP_TAGS.has(tag))
        return;
      if (!isRendered(element))
        return;

      if (tag === 'BR') {
        addText('\n', depth, inheritedRect);
        return;
      }

      const interactive = isInteractive(element);
      const level = HEADING_LEVELS[tag];
      const replaced = tag === 'IMG' || tag === 'IFRAME' || tag === 'FRAME';
      const container = CONTAINER_TAGS.has(tag);
      const form = tag === 'FORM';

      /* Fold inline text into the current run instead of emitting a node, but
       * keep recursing so an interactive descendant is still reached. */
      if (!interactive && !level && !replaced && !container && !form && INLINE_TAGS.has(tag)) {
        visitChildren(element, depth, inheritedRect);
        return;
      }

      /* Anything emitted, and any block boundary, ends the pending run. */
      flushRun();

      /* A boxless element reports an empty rect, which no region contains. Its
       * descendants measure themselves, but a text run sitting directly inside
       * it has only this rect to be placed by, so it inherits the enclosing
       * block's instead of being dropped. */
      let rect = element.getBoundingClientRect();
      if (inheritedRect && !rect.width && !rect.height && !rect.top && !rect.left)
        rect = inheritedRect;

      /* A <label> bound to a control contributes its text as that control's
       * label, so emitting the same words again as a text run is noise. Its
       * elements are still walked, since a label may wrap its own input. */
      const boundLabel = tag === 'LABEL' && element.control;

      if (interactive && !form) {
        const editable = isEditable(element);
        if (inRegion(rect))
          push(describe(element, depth, true, !editable));
        /* The element's own text is already folded into its node, so there is
         * nothing left to walk. Editable regions are the exception: their
         * content is the point. */
        if (!editable)
          return;
        visitChildren(element, depth + 1, rect, false);
        flushRun();
        return;
      }

      if (level) {
        /* A heading holding a link would lose that link's handle if the
         * heading swallowed its text, so only inline the text when there is
         * nothing interactive inside. */
        if (element.querySelector(INTERACTIVE_SELECTOR)) {
          if (inRegion(rect))
            push(describe(element, depth, false, false));
          visitChildren(element, depth + 1, rect, false);
          flushRun();
        } else if (inRegion(rect)) {
          push(describe(element, depth, false, true));
        }
        return;
      }

      if (replaced) {
        if (inRegion(rect))
          push(describe(element, depth, tag !== 'IMG', true));
        return;
      }

      if (form || (includeContainers && container)) {
        if (inRegion(rect))
          push(describe(element, depth, form, false));
        visitChildren(element, depth + 1, rect, false);
        flushRun();
        return;
      }

      visitChildren(element, depth, rect, boundLabel);
      flushRun();
    }

    const body = document.body;
    if (body)
      visitChildren(body, 1, body.getBoundingClientRect(), false);
    flushRun();

    const root = document.documentElement;
    return {
      url: location.href,
      title: document.title,
      region: wholeDocument ? 'document' : 'viewport',
      epoch,
      root: {
        scrollX: Math.round(window.scrollX),
        scrollY: Math.round(window.scrollY),
        width: root ? root.scrollWidth : 0,
        height: root ? root.scrollHeight : 0,
        viewportWidth: viewWidth,
        viewportHeight: viewHeight,
      },
      nodes,
      truncated,
    };
  };
})();
