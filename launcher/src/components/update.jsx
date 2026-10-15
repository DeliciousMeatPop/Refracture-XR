import { Loader2 } from 'lucide-react';
import { call } from '../api';
import { LogoMark } from './logo';

const megabytes = bytes => `${Math.round(bytes / 1e6)} MB`;
const plain = text => text.replace(/\*\*(.+?)\*\*/g, '$1').replace(/`([^`]+)`/g, '$1').replace(/\[([^\]]+)\]\([^)]+\)/g, '$1').trim();

// The release notes are GitHub Markdown; the dialog shows their "What's new" section (or all of them) as plain
// paragraphs and bullets.
function ReleaseNotes({ notes }) {
  const section = /^##[^\n]*what.s new[^\n]*\n([\s\S]*?)(?=^##\s|(?![\s\S]))/im.exec(notes)?.[1] ?? notes;
  const blocks = [];
  let fenced = false;
  for (const line of section.split(/\r?\n/)) {
    if (line.trim().startsWith('```')) { fenced = !fenced; continue; }
    if (fenced || !line.trim()) continue;
    const bullet = /^\s*[-*]\s+(.*)$/.exec(line);
    const heading = /^#+\s+(.*)$/.exec(line);
    if (bullet) {
      if (blocks.at(-1)?.type !== 'list') blocks.push({ type: 'list', items: [] });
      blocks.at(-1).items.push(plain(bullet[1]));
    } else blocks.push({ type: heading ? 'heading' : 'text', text: plain(heading ? heading[1] : line) });
  }
  return <div className="update-notes">
    {blocks.map((block, i) => block.type === 'list'
      ? <ul key={i}>{block.items.map((item, j) => <li key={j}>{item}</li>)}</ul>
      : block.type === 'heading' ? <strong key={i}>{block.text}</strong> : <p key={i}>{block.text}</p>)}
  </div>;
}

// Offered on each start when a newer Refract release is out. Updating downloads it, closes Refract, copies the new
// files in and starts it again; games, settings and Meta sign-in are kept.
export function UpdateDialog({ update, state, run, pending, onClose }) {
  const installing = pending.has('update') || Boolean(state.updateProgress);
  const install = () => run('update', () => call('installUpdate'));
  return <div className="welcome" role="dialog" aria-modal="true" aria-labelledby="update-title">
    <div className="welcome-card">
      <span className="orbit"><LogoMark /></span>
      <h1 id="update-title">{update.name} is available</h1>
      <p>You have Refract {state.version}. Do you want to update now? Your games, settings and Meta sign-in are kept.</p>
      {update.notes && <ReleaseNotes notes={update.notes} />}
      <div className="welcome-actions">
        {installing && <span className="update-progress" role="status">{state.updateProgress || 'Starting the update'}</span>}
        <button type="button" className="btn btn-ghost" disabled={installing} onClick={onClose}>Not now</button>
        <button type="button" className="btn btn-primary" disabled={installing} onClick={install}>
          {installing && <Loader2 className="spin" />}Update ({megabytes(update.size)})</button>
      </div>
    </div>
  </div>;
}
