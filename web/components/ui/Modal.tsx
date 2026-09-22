"use client";

import { useEffect, useRef } from "react";
import type { ReactNode, MouseEvent } from "react";
import { cn } from "@/lib/cn";

// A native <dialog>: focus trapping, Escape-to-close and the backdrop come
// for free from the browser, no extra dependency needed for something this
// self-contained.
export function Modal({
  open,
  onClose,
  title,
  children,
  className,
}: {
  open: boolean;
  onClose: () => void;
  title: ReactNode;
  children: ReactNode;
  className?: string;
}) {
  const ref = useRef<HTMLDialogElement>(null);

  useEffect(() => {
    const dialog = ref.current;
    if (!dialog) return;
    if (open && !dialog.open) dialog.showModal();
    else if (!open && dialog.open) dialog.close();
  }, [open]);

  // The dialog's own "close" event fires for Escape and for dialog.close()
  // alike, so this is the one place that needs to call onClose().
  useEffect(() => {
    const dialog = ref.current;
    if (!dialog) return;
    const handleClose = () => onClose();
    dialog.addEventListener("close", handleClose);
    return () => dialog.removeEventListener("close", handleClose);
  }, [onClose]);

  function handleBackdropClick(event: MouseEvent<HTMLDialogElement>) {
    if (event.target === ref.current) ref.current?.close();
  }

  return (
    <dialog
      ref={ref}
      onClick={handleBackdropClick}
      onCancel={(event) => {
        // Let the "close" listener above call onClose(); this only stops the
        // default cancel behavior from double-firing anything in callers.
        event.preventDefault();
        ref.current?.close();
      }}
      className={cn(
        "w-full max-w-lg rounded-lg border border-border bg-surface p-0 text-foreground shadow-xl backdrop:bg-transparent",
        className,
      )}
    >
      <div onClick={(e) => e.stopPropagation()}>
        <div className="flex items-center justify-between border-b border-border px-4 py-3">
          <h3 className="text-sm font-semibold">{title}</h3>
          <button
            type="button"
            onClick={() => ref.current?.close()}
            className="rounded p-1 text-muted hover:bg-border/50 hover:text-foreground"
            aria-label="Close"
          >
            ✕
          </button>
        </div>
        <div className="p-4">{children}</div>
      </div>
    </dialog>
  );
}
