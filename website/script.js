"use strict";

const menuButton = document.querySelector(".menu-toggle");
const sidebar = document.querySelector(".sidebar");
const navigationLinks = [...document.querySelectorAll(".nav-link")];
const sections = navigationLinks
  .map((link) => document.querySelector(link.getAttribute("href")))
  .filter(Boolean);

function closeMenu() {
  if (!menuButton || !sidebar) return;
  menuButton.setAttribute("aria-expanded", "false");
  sidebar.classList.remove("open");
}

menuButton?.addEventListener("click", () => {
  const isExpanded = menuButton.getAttribute("aria-expanded") === "true";
  menuButton.setAttribute("aria-expanded", String(!isExpanded));
  sidebar?.classList.toggle("open", !isExpanded);
});

navigationLinks.forEach((link) => {
  link.addEventListener("click", closeMenu);
});

const sectionObserver = new IntersectionObserver(
  (entries) => {
    const visibleEntries = entries
      .filter((entry) => entry.isIntersecting)
      .sort((first, second) => second.intersectionRatio - first.intersectionRatio);
    if (visibleEntries.length === 0) return;

    const currentId = visibleEntries[0].target.id;
    navigationLinks.forEach((link) => {
      const isCurrent = link.getAttribute("href") === `#${currentId}`;
      link.classList.toggle("active", isCurrent);
      if (isCurrent) link.setAttribute("aria-current", "location");
      else link.removeAttribute("aria-current");
    });
  },
  { rootMargin: "-15% 0px -70% 0px", threshold: [0, 0.1, 0.3] },
);

sections.forEach((section) => sectionObserver.observe(section));

document.querySelectorAll(".copy-button").forEach((button) => {
  button.addEventListener("click", async () => {
    const code = button.closest("pre")?.querySelector("code");
    if (!code) return;

    const originalLabel = button.textContent;
    try {
      await navigator.clipboard.writeText(code.innerText);
      button.textContent = "Copiado";
    } catch (error) {
      button.textContent = "Falhou";
      console.error("Could not copy the command to the clipboard.", error);
    }
    window.setTimeout(() => {
      button.textContent = originalLabel;
    }, 1600);
  });
});
