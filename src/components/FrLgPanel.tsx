import { useState, useCallback, useMemo, forwardRef, useImperativeHandle } from 'react';
import { useSettingsStore, DEFAULT_FRLG } from '../store';
import { GBA_MS_PER_FRAME, INT_MAX, INT_MIN } from '../utils/constants';
import { FormField } from './common/FormField';
import { IntInput } from './common/IntInput';
import { FloatInput } from './common/FloatInput';
import type { TimerPanelHandle } from './timerPanel';
import { useEsp32 } from '../hooks/useEsp32';
import type { Esp32Status } from '../hooks/useEsp32';

const FRAME_ADVANCES = 600;
// Duration to hold A when opening the continue screen (ms)
const A_HOLD_MS = 3000;
// Fixed navigation time after selecting save game:
//   A 0.1s + 0.8s wait + B 0.1s + 3s wait = 4000ms
//   A 0.3s + 1s + A 0.3s + 1s + A 0.3s = 2900ms
//   Total: 6900ms
const NAV_DURATION_MS = 6900;

function formatSeconds(ms: number): string {
  const str = (ms / 1000).toFixed(3);
  return str.replace(/\.?0+$/, '') + 's';
}

function toCliMacro(macro: string): string {
  const lines = macro
    .split('\n')
    .filter((line) => !line.startsWith('#') && line.trim() !== '');
  return `sudo nxbt macro -c "${lines.join('\\n')}"`;
}

function generateMacro(
  seedMs: number,
  seedCalibration: number,
  continueAdvances: number,
  continueCalibration: number,
): string {
  const calibratedSeedMs = seedMs + seedCalibration;
  const calibratedContinueMs = continueAdvances * GBA_MS_PER_FRAME + continueCalibration;
  const frameMs = FRAME_ADVANCES * GBA_MS_PER_FRAME;
  const continueWaitMs = Math.max(0, calibratedContinueMs - A_HOLD_MS);
  const frameWaitMs = Math.max(0, frameMs - NAV_DURATION_MS);

  return [
    `# FireRed/LeafGreen Shiny Starter`,
    `# Seed: ${seedMs}ms (cal ${seedCalibration >= 0 ? '+' : ''}${seedCalibration}) | Continue: ${continueAdvances} advances (cal ${continueCalibration >= 0 ? '+' : ''}${continueCalibration.toFixed(3)})`,
    ``,
    `# Setup: start game and open HOME menu`,
    `A 0.1s`,
    `1s`,
    `A 0.1s`,
    `1.5s`,
    `HOME 0.1s`,
    `20s`,
    ``,
    `# >>> Start EonTimer now + press A to enter game <<<`,
    `A 0.1s`,
    ``,
    `# Phase 1: Wait for seed timing, then open continue screen`,
    `${formatSeconds(calibratedSeedMs - 100)}`,
    `A ${formatSeconds(A_HOLD_MS)}`,
    ``,
    `# Phase 2: Wait for continue screen timer, then select save game`,
    `${formatSeconds(continueWaitMs)}`,
    `A 0.1s`,
    `0.8s`,
    ``,
    `# Skip previously on screen`,
    `B 0.1s`,
    `3s`,
    ``,
    `# Navigate to starter dialog`,
    `A 0.3s`,
    `1s`,
    `A 0.3s`,
    `1s`,
    `A 0.3s`,
    ``,
    `# Phase 3: Wait for frame timer (${FRAME_ADVANCES} advances), then frame-perfect press`,
    `${formatSeconds(frameWaitMs)}`,
    `A 0.1s`,
  ].join('\n');
}

const STATUS_LABELS: Record<Esp32Status, string> = {
  disconnected: 'Disconnected',
  connecting: 'Connecting...',
  connected: 'Connected (BLE not paired)',
  ble_connected: 'BLE Connected',
  ble_disconnected: 'BLE Disconnected',
  error: 'Connection error',
};

const STATUS_COLORS: Record<Esp32Status, string> = {
  disconnected: 'var(--color-text-muted)',
  connecting: '#f0b429',
  connected: 'var(--color-accent)',
  ble_connected: '#38a169',
  ble_disconnected: '#dd6b20',
  error: 'var(--color-danger)',
};

interface FrLgPanelProps {
  onPhasesChange: () => void;
  disabled?: boolean;
  onTrigger?: () => void;
}

export const FrLgPanel = forwardRef<TimerPanelHandle, FrLgPanelProps>(function FrLgPanel(
  { onPhasesChange, disabled, onTrigger },
  ref,
) {
  const frlg = useSettingsStore((s) => s.frlg);
  const updateFrLg = useSettingsStore((s) => s.updateFrLg);
  const [seedHit, setSeedHit] = useState<number | null>(null);
  const [continueHit, setContinueHit] = useState<number | null>(null);
  const [copied, setCopied] = useState(false);
  const [copiedCli, setCopiedCli] = useState(false);
  const [urlDraft, setUrlDraft] = useState(frlg.esp32Url);

  const { status, connect, disconnect, sendConfig, trigger } = useEsp32();
  const isConnected = status === 'connected' || status === 'ble_connected' || status === 'ble_disconnected';

  const macro = useMemo(
    () => generateMacro(frlg.seedMs, frlg.seedCalibration, frlg.continueAdvances, frlg.continueCalibration),
    [frlg.seedMs, frlg.seedCalibration, frlg.continueAdvances, frlg.continueCalibration],
  );

  const cliMacro = useMemo(() => toCliMacro(macro), [macro]);

  const createDisplayData = useCallback(() => {
    return {
      phases: [
        frlg.seedMs + frlg.seedCalibration,
        frlg.continueAdvances * GBA_MS_PER_FRAME + frlg.continueCalibration,
        FRAME_ADVANCES * GBA_MS_PER_FRAME,
      ],
      minutesBeforeTarget: null,
    };
  }, [frlg.seedMs, frlg.seedCalibration, frlg.continueAdvances, frlg.continueCalibration]);

  const canCalibrate = useCallback(
    () => seedHit !== null || continueHit !== null,
    [seedHit, continueHit],
  );

  const calibrate = useCallback(() => {
    const patch: Partial<typeof frlg> = {};
    if (seedHit !== null) {
      patch.seedCalibration = frlg.seedCalibration + (frlg.seedMs - seedHit);
      setSeedHit(null);
    }
    if (continueHit !== null) {
      patch.continueCalibration =
        frlg.continueCalibration + (frlg.continueAdvances - continueHit) * GBA_MS_PER_FRAME;
      setContinueHit(null);
    }
    updateFrLg(patch);
    sendConfig({ ...frlg, ...patch });
    setTimeout(onPhasesChange, 0);
  }, [frlg, seedHit, continueHit, updateFrLg, onPhasesChange, sendConfig]);

  const reset = useCallback(() => {
    updateFrLg({ ...DEFAULT_FRLG });
    setSeedHit(null);
    setContinueHit(null);
    setTimeout(onPhasesChange, 0);
  }, [updateFrLg, onPhasesChange]);

  useImperativeHandle(ref, () => ({ createDisplayData, calibrate, canCalibrate, reset }), [
    createDisplayData,
    calibrate,
    canCalibrate,
    reset,
  ]);

  const update = useCallback(
    (patch: Partial<typeof frlg>) => {
      updateFrLg(patch);
      setTimeout(onPhasesChange, 0);
    },
    [updateFrLg, onPhasesChange],
  );

  const handleCopy = useCallback(() => {
    navigator.clipboard.writeText(macro).then(() => {
      setCopied(true);
      setTimeout(() => setCopied(false), 2000);
    });
  }, [macro]);

  const handleCopyCli = useCallback(() => {
    navigator.clipboard.writeText(cliMacro).then(() => {
      setCopiedCli(true);
      setTimeout(() => setCopiedCli(false), 2000);
    });
  }, [cliMacro]);

  const handleConnect = useCallback(() => {
    updateFrLg({ esp32Url: urlDraft });
    connect(urlDraft);
  }, [urlDraft, connect, updateFrLg]);

  const handleTrigger = useCallback(() => {
    trigger();
    onTrigger?.();
  }, [trigger, onTrigger]);

  return (
    <div className="timer-panel frlg-panel">
      <div className="panel-scroll-area">
        <div className="panel-form-group">
          <FormField label="Seed Timer (ms)">
            <IntInput
              value={frlg.seedMs}
              onChange={(v) => update({ seedMs: v ?? 0 })}
              min={0}
              max={INT_MAX}
              disabled={disabled}
            />
          </FormField>
          <FormField label="Seed Calibration">
            <FloatInput
              value={frlg.seedCalibration}
              onChange={(v) => update({ seedCalibration: v })}
              min={INT_MIN}
              max={INT_MAX}
              disabled={disabled}
            />
          </FormField>
          <FormField label="Continue Screen Advances">
            <IntInput
              value={frlg.continueAdvances}
              onChange={(v) => update({ continueAdvances: v ?? 0 })}
              min={0}
              max={INT_MAX}
              disabled={disabled}
            />
          </FormField>
          <FormField label="Continue Calibration">
            <FloatInput
              value={frlg.continueCalibration}
              onChange={(v) => update({ continueCalibration: v })}
              min={INT_MIN}
              max={INT_MAX}
              disabled={disabled}
            />
          </FormField>
        </div>
      </div>
      <div className="panel-hit-fields">
        <FormField label="Seed Hit (ms)">
          <IntInput
            value={seedHit}
            onChange={setSeedHit}
            min={0}
            max={INT_MAX}
            allowBlank
            placeholder="Enter seed hit"
            disabled={disabled}
          />
        </FormField>
        <FormField label="Continue Hit (advances)">
          <IntInput
            value={continueHit}
            onChange={setContinueHit}
            min={0}
            max={INT_MAX}
            allowBlank
            placeholder="Enter continue hit"
            disabled={disabled}
          />
        </FormField>
      </div>
      <div className="frlg-esp32-section">
        <span className="frlg-esp32-header">ESP32 Controller</span>
        <div className="frlg-esp32-url-row">
          <input
            type="text"
            className="frlg-esp32-url-input"
            value={urlDraft}
            onChange={(e) => setUrlDraft(e.target.value)}
            placeholder="ws://192.168.1.x/ws"
            disabled={isConnected || status === 'connecting'}
            spellCheck={false}
          />
          <button
            className="btn"
            onClick={isConnected ? disconnect : handleConnect}
            disabled={status === 'connecting'}
          >
            {isConnected ? 'Disconnect' : 'Connect'}
          </button>
        </div>
        <div className="frlg-esp32-status">
          <span
            className="frlg-esp32-status-dot"
            style={{ background: STATUS_COLORS[status] }}
          />
          {STATUS_LABELS[status]}
        </div>
        <button
          className="btn frlg-esp32-trigger"
          onClick={handleTrigger}
          disabled={status !== 'ble_connected' || disabled}
        >
          Trigger + Start EonTimer
        </button>
        <span className="frlg-esp32-note">Requires HTTP origin — use npm run dev</span>
      </div>
      <div className="frlg-macro-section">
        <div className="frlg-macro-header">
          <span className="frlg-macro-label">NXBT Macro</span>
          <button className="btn" onClick={handleCopy}>
            {copied ? 'Copied!' : 'Copy'}
          </button>
        </div>
        <textarea className="frlg-macro-output" value={macro} readOnly spellCheck={false} />
      </div>
      <div className="frlg-macro-section">
        <div className="frlg-macro-header">
          <span className="frlg-macro-label">NXBT Macro (CLI)</span>
          <button className="btn" onClick={handleCopyCli}>
            {copiedCli ? 'Copied!' : 'Copy'}
          </button>
        </div>
        <textarea className="frlg-macro-output" value={cliMacro} readOnly spellCheck={false} />
      </div>
    </div>
  );
});
