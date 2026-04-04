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
  connect: (url: string) => void;
  disconnect: () => void;
  sendConfig: (config: Omit<FrLgSettings, 'esp32Url'>) => void;
  trigger: () => void;
}

export function useEsp32(): UseEsp32Return {
  const [status, setStatus] = useState<Esp32Status>('disconnected');
  const wsRef = useRef<WebSocket | null>(null);
  const intentionalClose = useRef(false);

  const send = useCallback((data: object) => {
    if (wsRef.current?.readyState === WebSocket.OPEN) {
      wsRef.current.send(JSON.stringify(data));
    }
  }, []);

  const disconnect = useCallback(() => {
    intentionalClose.current = true;
    wsRef.current?.close();
    wsRef.current = null;
    setStatus('disconnected');
  }, []);

  const connect = useCallback(
    (url: string) => {
      if (wsRef.current) {
        intentionalClose.current = true;
        wsRef.current.close();
      }
      intentionalClose.current = false;
      setStatus('connecting');

      const ws = new WebSocket(url);
      wsRef.current = ws;

      ws.onopen = () => {
        setStatus('connected');
      };

      ws.onmessage = (event) => {
        try {
          const msg = JSON.parse(event.data as string);
          if (msg.type === 'status') {
            const state: string = msg.state;
            if (state === 'ble_connected') setStatus('ble_connected');
            else if (state === 'ble_disconnected') setStatus('ble_disconnected');
            else if (state === 'idle' || state === 'running') setStatus('connected');
          }
        } catch {
          // ignore malformed messages
        }
      };

      ws.onerror = () => {
        setStatus('error');
      };

      ws.onclose = () => {
        if (!intentionalClose.current) {
          setStatus('disconnected');
        }
        wsRef.current = null;
      };
    },
    [],
  );

  const sendConfig = useCallback(
    (config: Omit<FrLgSettings, 'esp32Url'>) => {
      send({
        type: 'config',
        seedMs: config.seedMs,
        seedCalibration: config.seedCalibration,
        continueAdvances: config.continueAdvances,
        continueCalibration: config.continueCalibration,
      });
    },
    [send],
  );

  const trigger = useCallback(() => {
    send({ type: 'trigger' });
  }, [send]);

  useEffect(() => {
    return () => {
      intentionalClose.current = true;
      wsRef.current?.close();
    };
  }, []);

  return { status, connect, disconnect, sendConfig, trigger };
}
