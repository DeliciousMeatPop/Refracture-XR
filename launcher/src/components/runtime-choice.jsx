import { Cpu, Zap } from 'lucide-react';

export const runtimes = [
  { id: 'native', title: 'Refract Native', icon: Zap, tag: 'Recommended', recommended: true,
    text: 'Runs games natively on the PC, no emulator.' },
  { id: 'emulator', title: 'Android emulator', icon: Cpu, tag: 'Most compatible',
    text: 'Runs games in an Android emulator.' },
];

// Two cards to pick how Refract runs games. Used by the first-run wizard and Settings > Runtime.
export function RuntimeChoice({ value, onChange, disabled }) {
  return <div className="runtime-choice" role="radiogroup" aria-label="How Refract runs games">
    {runtimes.map(({ id, title, icon: Icon, tag, recommended, text }) => <button key={id} type="button" role="radio" aria-checked={value === id} disabled={disabled}
      className={`runtime-card ${value === id ? 'selected' : ''}`} onClick={() => onChange(id)}>
      <Icon /><div><strong>{title}</strong><small className={recommended ? 'recommended' : ''}>{tag}</small><span>{text}</span></div>
    </button>)}
  </div>;
}
