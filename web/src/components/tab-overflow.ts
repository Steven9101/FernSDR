/**
 * Which of the receiver's tabs fit the bar and which go behind its "More"
 * button. At most `limit` show: the first ones in their order, and in the
 * last place the active tab when it would be hidden, else the decodes, which
 * a listener on a decoded frequency is sent to, else the next in order.
 */
export function splitTabs<T extends { id: string }>(tabs: readonly T[], active: string, limit = 4): { shown: T[]; more: T[] } {
  if (tabs.length <= limit) return { shown: [...tabs], more: [] };
  const first = tabs.slice(0, limit - 1);
  const rest = tabs.slice(limit - 1);
  const featured = rest.find((tab) => tab.id === active) ?? rest.find((tab) => tab.id === 'decodes') ?? rest[0];
  return { shown: [...first, featured], more: rest.filter((tab) => tab !== featured) };
}
