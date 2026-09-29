// Sets the colour scheme before the first paint, so a dark-mode user never
// gets a white flash on load. A plain script, which blocks the parser until it
// has run, in a file of its own rather than inline so the page's
// Content-Security-Policy can allow scripts from this receiver alone.
(function () {
  try {
    var stored = JSON.parse(localStorage.getItem('fernsdr.preferences.v1') ?? '{}');
    var choice = stored.theme || 'auto';
    var dark = choice === 'dark' ||
      (choice === 'auto' && window.matchMedia('(prefers-color-scheme: dark)').matches);
    document.documentElement.dataset.colorScheme = dark ? 'dark' : 'light';
  } catch (error) {
    document.documentElement.dataset.colorScheme = 'dark';
  }
})();
