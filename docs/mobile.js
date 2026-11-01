// Menu v hlavičce na telefonu: tlačítko rozbalí odkazy pod hlavičkou,
// klepnutí na odkaz, mimo menu nebo Esc ho zavře
(() => {
  const btn = document.getElementById('menu-btn');
  const nav = document.getElementById('site-nav');
  if (!btn || !nav) return;
  const header = btn.closest('.site-header');
  const setOpen = (open) => {
    header.classList.toggle('menu-open', open);
    btn.setAttribute('aria-expanded', open ? 'true' : 'false');
  };
  btn.addEventListener('click', () => setOpen(!header.classList.contains('menu-open')));
  nav.addEventListener('click', (e) => { if (e.target.closest('a')) setOpen(false); });
  document.addEventListener('click', (e) => { if (!header.contains(e.target)) setOpen(false); });
  window.addEventListener('keydown', (e) => { if (e.key === 'Escape') setOpen(false); });
  window.addEventListener('resize', () => { if (window.innerWidth > 640) setOpen(false); });
})();
