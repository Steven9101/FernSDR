// Before the first paint, so a dark room never gets a white flash. A plain
// script, which blocks the parser until it has run, in a file of its own
// rather than inline so the page's Content-Security-Policy can allow scripts
// from this receiver alone.
(function () {
  try {
    var choice = localStorage.getItem('fernsdr.admin.appearance') || 'system';
    var dark = choice === 'dark' ||
      (choice !== 'light' && window.matchMedia('(prefers-color-scheme: dark)').matches);
    document.documentElement.classList.toggle('dark', dark);
  } catch (error) {
    document.documentElement.classList.add('dark');
  }
})();
