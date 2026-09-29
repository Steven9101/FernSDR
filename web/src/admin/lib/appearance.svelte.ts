/*
Light, dark, or whatever the system says, remembered per browser. The page's first paint already
follows the stored choice (a script in admin.html sets the class before anything renders); this
keeps it following after that, including when the system switches at dusk.
*/
export type AppearanceChoice = 'system' | 'light' | 'dark';

const KEY = 'fernsdr.admin.appearance';

function stored(): AppearanceChoice {
  try {
    const value = localStorage.getItem(KEY);
    return value === 'light' || value === 'dark' ? value : 'system';
  } catch {
    return 'system';
  }
}

const media = typeof matchMedia === 'function' ? matchMedia('(prefers-color-scheme: dark)') : null;

class Appearance {
  choice = $state<AppearanceChoice>(stored());
  systemDark = $state(media?.matches ?? true);
  resolved = $derived<'light' | 'dark'>(
    this.choice === 'system' ? (this.systemDark ? 'dark' : 'light') : this.choice,
  );

  set(choice: AppearanceChoice): void {
    this.choice = choice;
    try {
      localStorage.setItem(KEY, choice);
    } catch {
      // Storage switched off: the choice holds for this page load.
    }
    this.apply();
  }

  apply(): void {
    document.documentElement.classList.toggle('dark', this.resolved === 'dark');
  }
}

export const appearance = new Appearance();

media?.addEventListener('change', (event) => {
  appearance.systemDark = event.matches;
  appearance.apply();
});
