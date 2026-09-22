import { createTheme } from "@mantine/core";

// Mantine's design tokens, kept intentionally close to the defaults - this
// app has one job (manage a Lorentz instance), not a brand to express.
export const theme = createTheme({
  primaryColor: "blue",
  defaultRadius: "md",
  fontFamily: "var(--font-geist-sans), sans-serif",
  fontFamilyMonospace: "var(--font-geist-mono), monospace",
  headings: { fontFamily: "var(--font-geist-sans), sans-serif" },
});
