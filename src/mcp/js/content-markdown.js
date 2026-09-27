/* Render the page as Markdown.
 *
 * The text and json formats get the definition of rendered text for free from
 * innerText. This one walks the DOM itself, to keep headings, links, emphasis
 * and list structure, so it has to apply those rules on its own: script and
 * style bodies are not prose, and a hidden subtree is not content. */
(() => {
  const SKIP_TAGS = new Set(['script', 'style', 'noscript', 'template', 'head', 'meta', 'link', 'title', 'base',
                             'option', 'optgroup']);

  /* checkVisibility() is unsuitable here because it reports an element with no
   * box as invisible, which would discard the whole subtree of a
   * `display: contents` wrapper. */
  const isHidden = element => {
    const style = getComputedStyle(element);
    return style.display === 'none' || style.visibility === 'hidden' || style.contentVisibility === 'hidden'
      || style.opacity === '0';
  };

  const walk = node => {
    if (node.nodeType === 3)
      return node.textContent;
    if (node.nodeType !== 1)
      return '';
    const tag = node.tagName.toLowerCase();
    if (SKIP_TAGS.has(tag) || isHidden(node))
      return '';
    const content = Array.from(node.childNodes).map(walk).join('');
    if (/^h[1-6]$/.test(tag))
      return '\n' + '#'.repeat(+tag[1]) + ' ' + content.trim() + '\n';
    if (tag === 'a')
      return '[' + content.trim() + '](' + node.href + ')';
    if (tag === 'img')
      return '![' + (node.alt || '') + '](' + node.src + ')';
    if (tag === 'li')
      return '\n- ' + content.trim();
    if (tag === 'br')
      return '\n';
    if (['p', 'div', 'section', 'article', 'ul', 'ol', 'pre', 'blockquote'].includes(tag))
      return '\n' + content.trim() + '\n';
    if (tag === 'strong' || tag === 'b')
      return '**' + content + '**';
    if (tag === 'em' || tag === 'i')
      return '*' + content + '*';
    if (tag === 'code')
      return '`' + content + '`';
    return content;
  };
  return walk(document.body || document.documentElement).replace(/\n{3,}/g, '\n\n').trim();
})()
