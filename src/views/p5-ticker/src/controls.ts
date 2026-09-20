// Plain HTML <input type="range"> / hex-text-input / button-row controls,
// persisted to localStorage. Stands in for the Leva panel used in the
// original React ticker app.
//
// Colors and the font choice use hidden inputs plus on-page button/swatch
// rows rather than <input type="color"> or <select> — those open native OS
// popups that CEF's OSR-rendered content can't display. Worse than just
// being invisible, opening one still steals all keyboard input (you can't
// dismiss a menu you can't see), so we avoid triggering them at all.

export interface TickerSettings {
  textMessage: string;
  fontFamily: string;
  textSize: number;
  kerning: number;
  textYOffset: number;
  repeatPadding: number;
  scrollSpeed: number;
  textThresholdEnabled: boolean;
  textThreshold: number;
  textColor: string;
  bgColor: string;
}

const STORAGE_KEY = "p5-ticker-settings";
const SETTINGS_API = "http://localhost:5177/api/settings";

export const DEFAULT_SETTINGS: TickerSettings = {
  textMessage: "CHROMEYUMM|P5 TICKER DEMO|HELLO LED PANEL",
  fontFamily: "Arial",
  textSize: 27,
  kerning: 0,
  textYOffset: 0,
  repeatPadding: 80,
  scrollSpeed: -0.5,
  textThresholdEnabled: false,
  textThreshold: 0.5,
  textColor: "#ffffff",
  bgColor: "#000000",
};

function loadStoredSettings(): TickerSettings {
  try {
    const raw = localStorage.getItem(STORAGE_KEY);
    if (!raw) return { ...DEFAULT_SETTINGS };
    const parsed = JSON.parse(raw);
    return { ...DEFAULT_SETTINGS, ...parsed };
  } catch {
    // localStorage can throw (disabled storage, private mode, etc.) — fall
    // back to defaults rather than breaking the sketch.
    return { ...DEFAULT_SETTINGS };
  }
}

function saveStoredSettings(settings: TickerSettings) {
  try {
    localStorage.setItem(STORAGE_KEY, JSON.stringify(settings));
  } catch {
    // Ignore write failures — the sketch still works, it just won't persist.
  }
}

// The settings server (server.ts) is the real persistence layer — it writes
// to a JSON file on disk, sidestepping the CEF bug that keeps localStorage
// from surviving a Chromeyumm relaunch. localStorage above is kept only as
// an instant-first-paint fallback (no network round trip) and for when the
// page is opened outside Chromeyumm with the server not running.
async function loadServerSettings(): Promise<Partial<TickerSettings> | null> {
  try {
    const res = await fetch(SETTINGS_API);
    if (!res.ok) return null;
    return await res.json();
  } catch {
    return null; // server not reachable — not fatal, just no persistence this run
  }
}

function saveServerSettings(settings: TickerSettings) {
  fetch(SETTINGS_API, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(settings),
  }).catch(() => {
    // Best-effort — localStorage/in-memory state still works this session.
  });
}

function persist(settings: TickerSettings) {
  saveStoredSettings(settings);
  saveServerSettings(settings);
}

type FieldBinding<K extends keyof TickerSettings> = {
  key: K;
  el: HTMLInputElement | HTMLSelectElement;
  valueEl?: HTMLElement | null;
  parse: (raw: string) => TickerSettings[K];
  format?: (value: TickerSettings[K]) => string;
};

export function initControls(onChange: () => void): {
  settings: TickerSettings;
  panel: HTMLElement;
} {
  const settings = loadStoredSettings();
  const panel = document.getElementById("controls") as HTMLElement;

  const bindings: FieldBinding<keyof TickerSettings>[] = [
    {
      key: "textMessage",
      el: document.getElementById("textMessage") as HTMLInputElement,
      parse: (raw) => raw,
    },
    {
      key: "fontFamily",
      el: document.getElementById("fontFamily") as HTMLInputElement,
      parse: (raw) => raw,
    },
    {
      key: "textSize",
      el: document.getElementById("textSize") as HTMLInputElement,
      valueEl: document.getElementById("textSizeVal"),
      parse: Number,
      format: (v) => `${v}px`,
    },
    {
      key: "kerning",
      el: document.getElementById("kerning") as HTMLInputElement,
      valueEl: document.getElementById("kerningVal"),
      parse: Number,
      format: (v) => `${v}`,
    },
    {
      key: "textYOffset",
      el: document.getElementById("textYOffset") as HTMLInputElement,
      valueEl: document.getElementById("textYOffsetVal"),
      parse: Number,
      format: (v) => `${v}`,
    },
    {
      key: "repeatPadding",
      el: document.getElementById("repeatPadding") as HTMLInputElement,
      valueEl: document.getElementById("repeatPaddingVal"),
      parse: Number,
      format: (v) => `${v}`,
    },
    {
      key: "scrollSpeed",
      el: document.getElementById("scrollSpeed") as HTMLInputElement,
      valueEl: document.getElementById("scrollSpeedVal"),
      parse: Number,
      format: (v) => `${v}`,
    },
    {
      key: "textThresholdEnabled",
      el: document.getElementById("textThresholdEnabled") as HTMLInputElement,
      parse: (raw) => raw === "true",
    },
    {
      key: "textThreshold",
      el: document.getElementById("textThreshold") as HTMLInputElement,
      valueEl: document.getElementById("textThresholdVal"),
      parse: Number,
      format: (v) => `${v}`,
    },
    {
      key: "textColor",
      el: document.getElementById("textColor") as HTMLInputElement,
      parse: (raw) => raw,
    },
    {
      key: "bgColor",
      el: document.getElementById("bgColor") as HTMLInputElement,
      parse: (raw) => raw,
    },
  ];

  const colorKeys: (keyof TickerSettings)[] = ["textColor", "bgColor"];

  // Button/swatch rows standing in for <select> and <input type="color">.
  // Each group's buttons live in `.${rowClass}[data-for="<key>"]` and carry
  // their value in `data-${dataAttr}`.
  const buttonGroups: { keys: (keyof TickerSettings)[]; rowClass: string; btnClass: string; dataAttr: string }[] = [
    { keys: ["fontFamily"], rowClass: "option-row", btnClass: "option-btn", dataAttr: "value" },
    { keys: colorKeys, rowClass: "swatch-row", btnClass: "swatch", dataAttr: "color" },
  ];

  function syncColorPreviews() {
    for (const key of colorKeys) {
      const preview = document.getElementById(`${key}Preview`);
      if (preview) preview.style.backgroundColor = String(settings[key]);
    }
  }

  function syncActiveButtons() {
    for (const group of buttonGroups) {
      for (const key of group.keys) {
        const row = document.querySelector(`.${group.rowClass}[data-for="${key}"]`);
        row?.querySelectorAll<HTMLButtonElement>(`.${group.btnClass}`).forEach((btn) => {
          btn.classList.toggle("active", btn.dataset[group.dataAttr] === String(settings[key]));
        });
      }
    }
  }

  function applyToDom() {
    for (const b of bindings) {
      const value = settings[b.key];
      if (b.el instanceof HTMLInputElement && b.el.type === "checkbox") {
        b.el.checked = Boolean(value);
      } else {
        b.el.value = String(value);
      }
      if (b.valueEl) {
        b.valueEl.textContent = b.format ? b.format(value as never) : String(value);
      }
    }
    syncColorPreviews();
    syncActiveButtons();
  }

  for (const b of bindings) {
    const handler = () => {
      const raw = b.el instanceof HTMLInputElement && b.el.type === "checkbox" ? String(b.el.checked) : b.el.value;
      (settings as any)[b.key] = b.parse(raw);
      if (b.valueEl) {
        b.valueEl.textContent = b.format ? b.format(settings[b.key] as never) : raw;
      }
      if (colorKeys.includes(b.key)) syncColorPreviews();
      syncActiveButtons();
      persist(settings);
      onChange();
    };
    b.el.addEventListener("input", handler);
  }

  // Group buttons write into their backing input and fire the same "input"
  // event a real text field would, so they go through the normal
  // save/preview/active-state path above.
  for (const group of buttonGroups) {
    for (const key of group.keys) {
      const input = document.getElementById(key) as HTMLInputElement;
      document.querySelectorAll<HTMLButtonElement>(`.${group.rowClass}[data-for="${key}"] .${group.btnClass}`).forEach((btn) => {
        btn.addEventListener("click", () => {
          input.value = btn.dataset[group.dataAttr] ?? input.value;
          input.dispatchEvent(new Event("input", { bubbles: true }));
        });
      });
    }
  }

  const resetBtn = document.getElementById("resetDefaults");
  resetBtn?.addEventListener("click", () => {
    Object.assign(settings, DEFAULT_SETTINGS);
    applyToDom();
    persist(settings);
    onChange();
  });

  applyToDom();

  // Server settings load asynchronously — paint immediately with
  // localStorage/defaults above, then reconcile once the fetch resolves so
  // there's no blank/flashing UI while waiting on the network round trip.
  loadServerSettings().then((serverSettings) => {
    if (!serverSettings) return;
    Object.assign(settings, DEFAULT_SETTINGS, serverSettings);
    applyToDom();
    onChange();
  });

  return { settings, panel };
}
