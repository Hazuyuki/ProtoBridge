"use strict";

// English message IDs are shared by the browser and Python API. Native values,
// source files, logs and user names never pass through the translator.
const I18n = (() => {
  const storageKey = "protobridge.webui.language";
  let language = "en";
  let catalog = { en: {}, zh: {}, aliases: {} };
  let patterns = [];
  try {
    if (localStorage.getItem(storageKey) === "zh") language = "zh";
  } catch {
    // Storage can be unavailable in private or restricted browser contexts.
  }
  const format = (message, params) =>
    message.replace(/\{(\w+)\}/g, (match, key) =>
      Object.hasOwn(params, key) ? String(params[key]) : match,
    );
  function identify(message) {
    const key = catalog.aliases[message] || message;
    if (Object.hasOwn(catalog.en, key)) return { key, params: {} };
    for (const pattern of patterns) {
      const match = pattern.regex.exec(message);
      if (match)
        return {
          key: pattern.key,
          params: Object.fromEntries(
            pattern.names.map((name, i) => [name, match[i + 1]]),
          ),
        };
    }
    return { key: message, params: {} };
  }
  function t(message, params = {}) {
    const identified = identify(String(message ?? ""));
    return format(catalog[language][identified.key] || identified.key, {
      ...identified.params,
      ...params,
    });
  }
  function apply(root = document) {
    for (const node of root.querySelectorAll("[data-i18n]"))
      node.textContent = t(node.dataset.i18n);
    for (const attribute of ["title", "placeholder", "aria-label", "content"]) {
      for (const node of root.querySelectorAll(`[data-i18n-${attribute}]`))
        node.setAttribute(
          attribute,
          t(node.getAttribute(`data-i18n-${attribute}`)),
        );
    }
    document.documentElement.lang = language === "zh" ? "zh-CN" : "en";
    document.querySelectorAll("[data-language]").forEach((button) => {
      button.setAttribute(
        "aria-pressed",
        String(button.dataset.language === language),
      );
    });
  }
  function setLanguage(value) {
    if (!["en", "zh"].includes(value) || value === language) return;
    language = value;
    try {
      localStorage.setItem(storageKey, language);
    } catch {
      /* In-memory preference still works. */
    }
    apply();
    document.dispatchEvent(new CustomEvent("languagechange"));
  }
  const script = document.currentScript;
  const ready = fetch(script.dataset.catalog, { cache: "no-cache" })
    .then((response) => {
      if (!response.ok) throw new Error("Translation catalog unavailable");
      return response.json();
    })
    .then((data) => {
      catalog = data;
      const templates = new Map([
        ...Object.keys(data.en).map((key) => [key, key]),
        ...Object.entries(data.zh).map(([key, value]) => [value, key]),
        ...Object.entries(data.aliases),
      ]);
      patterns = [...templates]
        .filter(([source]) => /\{\w+\}/.test(source))
        .map(([source, key]) => {
          const names = [];
          let expression = "",
            cursor = 0;
          for (const match of source.matchAll(/\{(\w+)\}/g)) {
            expression +=
              source
                .slice(cursor, match.index)
                .replace(/[.*+?^${}()|[\]\\]/g, "\\$&") + "([\\s\\S]+?)";
            names.push(match[1]);
            cursor = match.index + match[0].length;
          }
          expression += source
            .slice(cursor)
            .replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
          return {
            key,
            names,
            regex: new RegExp("^" + expression + "$"),
            specificity: source.length - names.length * 8,
          };
        })
        .sort((a, b) => b.specificity - a.specificity);
    })
    .catch(() => {
      language = "en";
      document.querySelectorAll("[data-language]").forEach((button) => {
        button.disabled = true;
      });
      console.warn("Could not load translations. Using English.");
    })
    .then(() => {
      apply();
      document
        .querySelectorAll("[data-language]")
        .forEach((button) =>
          button.addEventListener("click", () =>
            setLanguage(button.dataset.language),
          ),
        );
    });
  return {
    t,
    apply,
    setLanguage,
    ready,
    get language() {
      return language;
    },
  };
})();
const t = (message, params) => I18n.t(message, params);
