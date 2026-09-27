"use client";

import { useEffect, useRef, useState } from "react";
import { Alert, Button, Code, Group, Loader, Modal, Stack, Text } from "@mantine/core";
import { IconAlertCircle, IconCheck } from "@tabler/icons-react";
import { apiStream } from "@/lib/client";

interface Props {
  opened: boolean;
  onClose: () => void;
  // The run ended, with or without lists updated: the table has changed
  onDone: () => void;
}

// Starts a gravity update as it opens and shows what the server reports about
// each list while it downloads
export function UpdateListsModal({ opened, onClose, onDone }: Props) {
  const [log, setLog] = useState("");
  const [running, setRunning] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const started = useRef(false);
  const box = useRef<HTMLPreElement>(null);

  useEffect(() => {
    if (!opened) {
      started.current = false;
      return;
    }
    if (started.current) return;
    started.current = true;
    setLog("");
    setError(null);
    setRunning(true);
    apiStream("/action/gravity", (text) => setLog((prev) => prev + text), { method: "POST" })
      .then(({ status }) => {
        if (status === 409) setError("An update is already running.");
        else if (status === 501) setError("This build of Lorentz cannot download lists.");
        else if (status >= 400) setError("The update failed.");
      })
      .catch(() => setError("The connection to Lorentz was lost."))
      .finally(() => {
        setRunning(false);
        onDone();
      });
  }, [opened, onDone]);

  useEffect(() => {
    box.current?.scrollTo({ top: box.current.scrollHeight });
  }, [log]);

  // The status answer that ends the stream is JSON, not part of the report
  const report = log.replace(/\{[\s\S]*\}\s*$/, "").trimEnd();
  const finished = !running && !error && /^Done:/m.test(report);

  return (
    <Modal opened={opened} onClose={running ? () => undefined : onClose} closeOnClickOutside={!running}
      withCloseButton={!running} title="Updating the lists" size="lg">
      <Stack>
        <Text size="sm" c="dimmed">
          Each enabled list is downloaded and replaces what it had; a list that cannot be downloaded keeps
          its domains.
        </Text>
        <Code block ref={box} mih={120} mah={320} style={{ overflow: "auto", whiteSpace: "pre-wrap" }}>
          {report || (running ? "Starting..." : "")}
        </Code>
        {error && (
          <Alert color="red" icon={<IconAlertCircle size={16} />}>
            {error}
          </Alert>
        )}
        {finished && (
          <Alert color="green" icon={<IconCheck size={16} />}>
            Done.
          </Alert>
        )}
        <Group justify="flex-end">
          {running && <Loader size="sm" />}
          <Button onClick={onClose} disabled={running}>
            Close
          </Button>
        </Group>
      </Stack>
    </Modal>
  );
}
