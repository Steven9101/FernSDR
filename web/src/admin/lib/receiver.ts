/**
 * What the setup flow and a restored backup both do to the receiver: fetch a
 * module from the catalog and wait for it, and restart and wait for the
 * receiver to answer again.
 */
import { api, type ModulesView } from '../api';

/** The modules view once no module job is queued or running. */
export async function waitForJobs(): Promise<ModulesView> {
  for (let i = 0; i < 600; i++) {
    const view = await api.modules();
    if (!view.jobs.some((job) => job.state === 'queued' || job.state === 'running')) return view;
    await new Promise((resolve) => setTimeout(resolve, 1000));
  }
  throw new Error('The receiver is still busy with modules; try again in a minute.');
}

/**
 * Installs the newest release of module `id` for this machine from the
 * catalog, from `repository` when that is one of the catalog's. Resolves to
 * the view after it, or throws with what went wrong in words for the operator.
 */
export async function installFromCatalog(id: string, name: string, repository?: string): Promise<ModulesView> {
  let view = await api.modules();
  const find = () => {
    const releases = view.available.flatMap((entry) =>
      (entry.releases ?? []).map((release) => ({ repository: entry, release })),
    );
    const usable = releases.filter(({ release }) => release.id === id && release.asset && !release.prerelease);
    return usable.find((found) => found.repository.repository === repository) ?? usable[0];
  };
  if (!find()) {
    await api.checkModules();
    view = await waitForJobs();
  }
  const found = find();
  if (!found) throw new Error(`No ${name} module for this computer was found in the module catalog.`);
  await api.installModule(found.repository.repository, found.release.tag, found.release.asset!, true);
  view = await waitForJobs();
  if (!view.installed.some((module) => module.id === id)) {
    const failed = view.jobs.find((job) => job.state === 'failed');
    throw new Error(failed?.message ?? `The ${name} module could not be installed.`);
  }
  return view;
}

/**
 * Restarts the receiver and reloads the page once it answers again; sessions
 * end with the receiver, so the page signs in afresh. Resolves to the reason
 * instead when the receiver cannot restart itself, as when it runs by hand.
 */
export async function restartAndReload(): Promise<string | undefined> {
  const hardware = await api.hardware();
  if (!hardware.can_restart) return hardware.restart_note ?? 'Restart FernSDR on the machine.';
  await api.restart();
  await new Promise((resolve) => setTimeout(resolve, 1500));
  for (let i = 0; i < 90; i++) {
    try {
      const answer = await fetch('/api/admin/session', { cache: 'no-store' });
      if (answer.ok) break;
    } catch {
      // Not back yet.
    }
    await new Promise((resolve) => setTimeout(resolve, 1000));
  }
  location.reload();
  return undefined;
}
