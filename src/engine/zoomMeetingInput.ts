export type ZoomMeetingJoinIdentity = {
  meetingNumber: string;
  passcode?: string;
};

export function parseZoomMeetingJoinInput(input: string): ZoomMeetingJoinIdentity | undefined {
  const trimmed = input.trim();
  if (!trimmed) {
    return undefined;
  }

  const digitsOnly = trimmed.replace(/\s+/g, "");
  if (/^\d{6,}$/.test(digitsOnly)) {
    return { meetingNumber: digitsOnly };
  }

  try {
    const url = new URL(trimmed);
    const pathMatch = url.pathname.match(/\/(?:j|wc\/join|my)\/(\d{6,})/i);
    const meetingNumber = pathMatch?.[1] ?? url.searchParams.get("confno") ?? undefined;
    if (!meetingNumber || !/^\d{6,}$/.test(meetingNumber)) {
      return undefined;
    }
    const passcode = url.searchParams.get("pwd") ?? url.searchParams.get("passcode") ?? undefined;
    return passcode ? { meetingNumber, passcode } : { meetingNumber };
  } catch {
    const embeddedNumber = trimmed.match(/\b(\d{6,})\b/)?.[1];
    return embeddedNumber ? { meetingNumber: embeddedNumber } : undefined;
  }
}
