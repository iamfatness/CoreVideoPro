export type CaptionBrokerCue = {
  id: string;
  text: string;
  atMs: number;
  speaker?: string;
  confidence?: number;
  isFinal?: boolean;
};

export type CaptionBrokerSession = {
  brokerSessionId: string;
  productionSessionId: string;
  language?: string;
};