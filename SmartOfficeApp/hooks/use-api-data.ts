import { useFocusEffect } from 'expo-router';
import { useCallback, useRef, useState } from 'react';

export function useApiData<T>(loadData: () => Promise<T>) {
  const [data, setData] = useState<T | null>(null);
  const [error, setError] = useState<Error | null>(null);
  const [isLoading, setIsLoading] = useState(true);
  const requestId = useRef(0);

  const load = useCallback(async () => {
    const currentRequestId = ++requestId.current;
    setIsLoading(true);
    setError(null);
    setData(null);

    try {
      const nextData = await loadData();
      if (requestId.current === currentRequestId) {
        setData(nextData);
      }
    } catch (nextError) {
      if (requestId.current === currentRequestId) {
        setError(nextError instanceof Error ? nextError : new Error('Unable to load data.'));
      }
    } finally {
      if (requestId.current === currentRequestId) {
        setIsLoading(false);
      }
    }
  }, [loadData]);

  useFocusEffect(
    useCallback(() => {
      void load();
      return () => {
        requestId.current += 1;
      };
    }, [load]),
  );

  return { data, error, isLoading, retry: load };
}
