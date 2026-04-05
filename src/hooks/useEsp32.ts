import { useState, useRef, useEffect, useCallback } from 'react';
import type { FrLgSettings } from '../store';

export type Esp32Status =
  | 'disconnected'
  | 'connecting'
  | 'connected'
  | 'ble_connected'
  | 'ble_disconnected'
  | 'error';

export interface UseEsp32Return {
  status: Esp32Status;
  connect: () => void;
  disconnect: () => void;
  sendConfig: (config: Omit<FrLgSettings, 'esp32Url'>) => void;
  trigger: () => void;
  onPhase1Start: (cb: (() => void) | null) => void;
}

// Web Serial API types (not yet in all TS libs)
declare global {
  interface Navigator {
    serial: {
      requestPort(options?: { filters?: { usbVendorId?: number }[] }): Promise<SerialPort>;
    };
  }
  interface SerialPort {
    open(options: { baudRate: number }): Promise<void>;
    close(): Promise<void>;
    readable: ReadableStream<Uint8Array> | null;
    writable: WritableStream<Uint8Array> | null;
  }
}

export function useEsp32(): UseEsp32Return {
  const [status, setStatus] = useState<Esp32Status>('disconnected');
  const portRef    = useRef<SerialPort | null>(null);
  const writerRef  = useRef<WritableStreamDefaultWriter<Uint8Array> | null>(null);
  const phase1Cb   = useRef<(() => void) | null>(null);
  const closingRef = useRef(false);

  const sendLine = useCallback((obj: object) => {
    if (!writerRef.current) return;
    const line = JSON.stringify(obj) + '\n';
    const bytes = new TextEncoder().encode(line);
    writerRef.current.write(bytes).catch(() => {});
  }, []);

  const disconnect = useCallback(() => {
    closingRef.current = true;
    writerRef.current?.close().catch(() => {});
    writerRef.current = null;
    portRef.current?.close().catch(() => {});
    portRef.current = null;
    setStatus('disconnected');
  }, []);

  const connect = useCallback(async () => {
    if (!('serial' in navigator)) {
      setStatus('error');
      return;
    }
    try {
      closingRef.current = false;
      setStatus('connecting');

      const port = await navigator.serial.requestPort();
      await port.open({ baudRate: 115200 });
      portRef.current = port;

      const writer = port.writable!.getWriter();
      writerRef.current = writer;
      setStatus('connected');

      // Ask the ESP32 for its current BT state
      sendLine({ type: 'getStatus' });

      // Read lines from the ESP32
      const reader = port.readable!.getReader();
      const decoder = new TextDecoder();
      let buf = '';

      (async () => {
        try {
          while (true) {
            const { value, done } = await reader.read();
            if (done) break;
            buf += decoder.decode(value, { stream: true });
            const lines = buf.split('\n');
            buf = lines.pop()!;
            for (const line of lines) {
              const trimmed = line.trim();
              if (!trimmed.startsWith('{')) continue;
              try {
                const msg = JSON.parse(trimmed);
                if (msg.type === 'status') {
                  const state: string = msg.state;
                  if (state === 'ble_connected')    setStatus('ble_connected');
                  else if (state === 'ble_disconnected') setStatus('ble_disconnected');
                  else if (state === 'phase1_start') phase1Cb.current?.();
                  else if (state === 'idle' || state === 'running') setStatus('connected');
                }
              } catch { /* ignore malformed JSON */ }
            }
          }
        } catch {
          // port closed or lost
        } finally {
          reader.releaseLock();
          if (!closingRef.current) {
            writerRef.current = null;
            portRef.current = null;
            setStatus('disconnected');
          }
        }
      })();

    } catch {
      setStatus('error');
    }
  }, [sendLine]);

  const sendConfig = useCallback(
    (config: Omit<FrLgSettings, 'esp32Url'>) => {
      sendLine({
        type: 'config',
        seedMs: config.seedMs,
        seedCalibration: config.seedCalibration,
        continueAdvances: config.continueAdvances,
        continueCalibration: config.continueCalibration,
      });
    },
    [sendLine],
  );

  const trigger = useCallback(() => {
    sendLine({ type: 'trigger' });
  }, [sendLine]);

  useEffect(() => {
    return () => {
      closingRef.current = true;
      writerRef.current?.close().catch(() => {});
      portRef.current?.close().catch(() => {});
    };
  }, []);

  const onPhase1Start = useCallback((cb: (() => void) | null) => {
    phase1Cb.current = cb;
  }, []);

  return { status, connect, disconnect, sendConfig, trigger, onPhase1Start };
}
