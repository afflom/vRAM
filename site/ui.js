// Apply a saved choice before paint. The stylesheet follows the OS in system mode.
(() => {
  const key = "vram-color-theme";
  const choices = ["light", "dark", "system"];
  function apply(value) {
    const choice = choices.includes(value) ? value : "system";
    document.documentElement.dataset.theme = choice;
    return choice;
  }
  let initial;
  try { initial = localStorage.getItem(key); } catch { /* Storage can be restricted. */ }
  apply(initial);
  document.addEventListener("DOMContentLoaded", () => {
    const select = document.getElementById("theme");
    if (!select) return;
    select.value = document.documentElement.dataset.theme;
    select.closest("label").hidden = false;
    select.addEventListener("change", () => {
      const choice = apply(select.value);
      try { localStorage.setItem(key, choice); } catch { /* The tab still honors the choice. */ }
    });
    window.addEventListener("storage", event => {
      if (event.key === key || event.key === null) select.value = apply(event.newValue);
    });
  });
})();
