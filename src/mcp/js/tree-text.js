/* Render a walked node list as indented text.
 *
 * One line per node, tab-indented by depth, with attributes as bare
 * "key=value" pairs and free text quoted. The header line carries the scroll
 * position and content size so a client can tell what it is looking at and
 * whether anything was cut.
 *
 * This is the first point at which the real size of the output is known --
 * indentation, quoting, and attribute names together add well over half again
 * to the raw field content -- so the byte ceiling belongs here. Lines are
 * measured encoded, since a budget counted in UTF-16 units undercounts by up to
 * three times on non-Latin text. */
(() => {
  const NS = (globalThis.__wig ??= {});
  if (NS.text)
    return;

  const encoder = new TextEncoder();

  const quote = text => "'" + String(text)
    .replace(/\\/g, '\\\\')
    .replace(/'/g, "\\'")
    .replace(/\n/g, '\\n') + "'";

  NS.text = (result, options) => {
    const root = result.root;
    const maxBytes = Math.max(1024, Number((options || {}).maxBytes) || 262144);

    const lines = [];
    let bytes = 0;
    let truncated = result.truncated;

    for (const node of result.nodes) {
      const parts = [];
      if (node.kind !== 'text')
        parts.push(node.kind);
      if (node.uid)
        parts.push('uid=' + node.uid);
      if (node.level)
        parts.push('level=' + node.level);
      if (node.label)
        parts.push('label=' + quote(node.label));
      if (node.url)
        parts.push('url=' + node.url);
      if (node.value !== undefined)
        parts.push('value=' + quote(node.value));
      if (node.checked !== undefined)
        parts.push(node.checked ? 'checked' : 'unchecked');
      if (node.expanded !== undefined)
        parts.push(node.expanded ? 'expanded' : 'collapsed');
      if (node.disabled)
        parts.push('disabled');
      if (node.required)
        parts.push('required');
      if (node.focused)
        parts.push('focused');
      if (node.text)
        parts.push(quote(node.text));

      const line = '\t'.repeat(node.depth) + parts.join(' ');
      const size = encoder.encode(line).length + 1;
      if (bytes + size > maxBytes) {
        truncated = 'bytes';
        break;
      }
      bytes += size;
      lines.push(line);
    }

    /* Built last: how much fits is only known once the lines exist. */
    let header = 'root scroll=(' + root.scrollX + ',' + root.scrollY + ')'
      + ' content=[' + root.width + '×' + root.height + ']'
      + ' viewport=[' + root.viewportWidth + '×' + root.viewportHeight + ']'
      + ' region=' + result.region;
    if (truncated === 'bytes')
      header += ' truncated=bytes (' + lines.length + ' of ' + result.nodes.length + ' nodes shown)';
    else if (truncated)
      header += ' truncated=' + truncated + ' (' + lines.length + ' nodes shown, more remain)';

    return header + '\n' + lines.join('\n');
  };
})();
