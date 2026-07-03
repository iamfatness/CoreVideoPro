import type { SomeCompanionConfigField } from '@companion-module/base'
import { Regex } from '@companion-module/base'

export interface CvpConfig {
	host: string
	httpPort: number
	token: string
}

export function getConfigFields(): SomeCompanionConfigField[] {
	return [
		{
			type: 'static-text',
			id: 'info',
			width: 12,
			label: 'CoreVideo Pro remote control',
			value:
				'Drives CoreVideo Pro over its HTTP/WebSocket control API. Enable it in the app (it listens on 127.0.0.1:8011 by default; set COREVIDEO_OSC_LAN=1 for LAN and COREVIDEO_CONTROL_TOKEN for a token). Actions are generated live from the app manifest; feedback streams over the WebSocket.',
		},
		{
			type: 'textinput',
			id: 'host',
			label: 'Host',
			width: 6,
			default: '127.0.0.1',
			regex: Regex.HOSTNAME,
		},
		{
			type: 'number',
			id: 'httpPort',
			label: 'HTTP port',
			width: 3,
			default: 8011,
			min: 1,
			max: 65535,
		},
		{
			type: 'textinput',
			id: 'token',
			label: 'Bearer token (optional)',
			width: 12,
			default: '',
		},
	]
}
