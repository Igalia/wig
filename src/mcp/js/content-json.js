JSON.stringify({
  url: location.href,
  title: document.title,
  text: document.body ? document.body.innerText : '',
  links: Array.from(document.links).map(a => ({ text: a.innerText, href: a.href })),
})
