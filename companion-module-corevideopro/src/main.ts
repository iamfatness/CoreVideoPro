import {
	InstanceBase,
	InstanceStatus,
	runEntrypoint,
	combineRgb,
	type CompanionActionDefinitions,
	type CompanionActionDefinition,
	type SomeCompanionActionInputField,
} from '@companion-module/base'
import WebSocket from 'ws'
import { getConfigFields, type CvpConfig } from './config.js'
import { buildFeedbacks, FEEDBACK_FLAGS } from './feedbacks.js'
import { buildVariables, VARIABLE_FIELDS } from './variables.js'
import { buildPresets } from './presets.js'

interface ManifestParam {
	name: string
	type: 'string' | 'int' | 'double' | 'bool'
	required: boolean
	description?: string
}
interface ManifestAction {
	id: string
	title: string
	description: string
	oscAddress: string
	params: ManifestParam[]
}
interface Manifest {
	version: number
	root: string
	actions: ManifestAction[]
	feedbackFields: string[]
}

class CvpInstance extends InstanceBase<CvpConfig> {
	private config!: CvpConfig
	private ws?: WebSocket
	private wsReconnect?: NodeJS.Timeout
	private state: Record<string, unknown> = {}
	private destroyed = false

	async init(config: CvpConfig): Promise<void> {
		this.config = config
		this.destroyed = false
		this.updateStatus(InstanceStatus.Connecting)

		// Static feedbacks/variables are stable (ControlState shape); actions come from the manifest.
		this.setFeedbackDefinitions(buildFeedbacks(this))
		this.setVariableDefinitions(buildVariables())
		this.setPresetDefinitions(buildPresets())

		await this.loadManifest()
		this.connectWebSocket()
	}

	async destroy(): Promise<void> {
		this.destroyed = true
		if (this.wsReconnect) clearTimeout(this.wsReconnect)
		this.ws?.close()
		this.ws = undefined
	}

	async configUpdated(config: CvpConfig): Promise<void> {
		await this.destroy()
		await this.init(config)
	}

	getConfigFields() {
		return getConfigFields()
	}

	private baseUrl(): string {
		return `http://${this.config.host}:${this.config.httpPort}`
	}

	private headers(): Record<string, string> {
		const h: Record<string, string> = { 'Content-Type': 'application/json' }
		if (this.config.token) h['Authorization'] = `Bearer ${this.config.token}`
		return h
	}

	// --- Actions (generated from GET /manifest) ---------------------------------------------

	private async loadManifest(): Promise<void> {
		try {
			const res = await fetch(`${this.baseUrl()}/manifest`, { headers: this.headers() })
			if (!res.ok) throw new Error(`HTTP ${res.status}`)
			const manifest = (await res.json()) as Manifest
			this.setActionDefinitions(this.buildActions(manifest))
		} catch (err) {
			this.log('warn', `Could not load manifest: ${String(err)}`)
			// Retry via the WS reconnect loop; keep whatever actions we already have.
		}
	}

	private buildActions(manifest: Manifest): CompanionActionDefinitions {
		const defs: CompanionActionDefinitions = {}
		for (const action of manifest.actions) {
			const options = action.params.map((p) => this.paramToOption(p))
			const def: CompanionActionDefinition = {
				name: action.title,
				description: action.description,
				options,
				callback: async (event) => {
					const args = action.params.map((p) => this.coerceOption(event.options[p.name], p))
					await this.invoke(action.id, args)
				},
			}
			defs[action.id] = def
		}
		return defs
	}

	private paramToOption(p: ManifestParam): SomeCompanionActionInputField {
		const id = p.name
		const label = p.description ? `${p.name} (${p.description})` : p.name
		switch (p.type) {
			case 'bool':
				return { id, label, type: 'checkbox', default: false }
			case 'int':
			case 'double':
				return { id, label, type: 'number', default: 0, min: -100000, max: 100000 }
			default:
				return { id, label, type: 'textinput', default: '', useVariables: true }
		}
	}

	private coerceOption(value: unknown, p: ManifestParam): unknown {
		switch (p.type) {
			case 'bool':
				return Boolean(value)
			case 'int':
				return Math.round(Number(value) || 0)
			case 'double':
				return Number(value) || 0
			default:
				return value == null ? '' : String(value)
		}
	}

	private async invoke(action: string, args: unknown[]): Promise<void> {
		try {
			const res = await fetch(`${this.baseUrl()}/invoke`, {
				method: 'POST',
				headers: this.headers(),
				body: JSON.stringify({ action, args }),
			})
			if (!res.ok) {
				const body = await res.text().catch(() => '')
				this.log('warn', `Action ${action} failed: HTTP ${res.status} ${body}`)
			}
		} catch (err) {
			this.log('error', `Action ${action} error: ${String(err)}`)
			this.scheduleReconnect()
		}
	}

	// --- Feedback (streamed over /ws) -------------------------------------------------------

	private connectWebSocket(): void {
		if (this.destroyed) return
		const token = this.config.token ? `?token=${encodeURIComponent(this.config.token)}` : ''
		const url = `ws://${this.config.host}:${this.config.httpPort}/ws${token}`
		try {
			this.ws = new WebSocket(url)
		} catch (err) {
			this.log('error', `WS connect error: ${String(err)}`)
			this.scheduleReconnect()
			return
		}

		this.ws.on('open', () => {
			this.updateStatus(InstanceStatus.Ok)
			// Refresh actions in case the app was restarted with a newer manifest.
			void this.loadManifest()
		})
		this.ws.on('message', (data) => this.onStateMessage(data.toString()))
		this.ws.on('close', () => {
			this.updateStatus(InstanceStatus.Disconnected)
			this.scheduleReconnect()
		})
		this.ws.on('error', (err) => {
			this.log('debug', `WS error: ${String(err)}`)
			this.updateStatus(InstanceStatus.ConnectionFailure)
		})
	}

	private scheduleReconnect(): void {
		if (this.destroyed || this.wsReconnect) return
		this.wsReconnect = setTimeout(() => {
			this.wsReconnect = undefined
			this.connectWebSocket()
		}, 2000)
	}

	private onStateMessage(text: string): void {
		let next: Record<string, unknown>
		try {
			next = JSON.parse(text)
		} catch {
			return
		}
		this.state = next

		// Publish string/number state as Companion variables.
		const values: Record<string, string | number | boolean> = {}
		for (const field of VARIABLE_FIELDS) {
			const v = next[field.id]
			if (v !== undefined && v !== null) values[field.id] = v as string | number | boolean
		}
		this.setVariableValues(values)

		// Re-evaluate the boolean feedbacks that light buttons.
		this.checkFeedbacks(...FEEDBACK_FLAGS.map((f) => f.id))
	}

	/** Read a boolean state field for a feedback (used by feedbacks.ts). */
	public flag(id: string): boolean {
		return Boolean(this.state[id])
	}
}

runEntrypoint(CvpInstance, [])

export { CvpInstance }
export const RED = combineRgb(200, 40, 40)
