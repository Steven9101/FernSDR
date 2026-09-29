/**
 * The offer of a newer FernSDR after signing in: the receiver is asked to
 * look once a day while the panel is open, never to install. Resolves to the
 * view to offer, or null when there is nothing new, the newest was put away,
 * or this receiver does not update itself.
 */
import { api, type UpdateView } from '../api';
import { lastAsked, lookDue, noteAsked, updateDismissed, updating } from './updates';

export async function updateToOffer(): Promise<UpdateView | null> {
  let view = await api.updates();
  if (!view.available || updating(view.status)) return null;
  const now = Math.floor(Date.now() / 1000);
  if (view.check.state !== 'checking' && lookDue(view.check.checked, now, lastAsked())) {
    noteAsked(now);
    view = await api.checkUpdates();
  }
  for (let i = 0; i < 40 && view.check.state === 'checking'; i++) {
    await new Promise((resolve) => setTimeout(resolve, 1500));
    view = await api.updates();
  }
  const found = view.check;
  if (found.state !== 'done' || !found.newer || !found.version || updateDismissed(found.version)) return null;
  return view;
}
