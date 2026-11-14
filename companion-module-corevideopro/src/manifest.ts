export interface ManifestParam {
	name: string
	type: 'string' | 'int' | 'double' | 'bool'
	required: boolean
	description?: string
}
export interface ManifestAction {
	id: string
	title: string
	description: string
	oscAddress: string
	params: ManifestParam[]
}
export interface Manifest {
	version: number
	root: string
	actions: ManifestAction[]
	feedbackFields: string[]
}

type Obj = Record<string, unknown>
const isObj = (v: unknown): v is Obj => typeof v === 'object' && v !== null && !Array.isArray(v)

/** Read `key` in camelCase or PascalCase (the Windows/Mac servers serve the manifest PascalCase). */
function pick(o: Obj, key: string): unknown {
	if (key in o) return o[key]
	const pascal = key.charAt(0).toUpperCase() + key.slice(1)
	return o[pascal]
}

function str(v: unknown, fallback = ''): string {
	return typeof v === 'string' ? v : fallback
}

function normalizeParam(raw: unknown): ManifestParam | undefined {
	if (!isObj(raw)) return undefined
	const name = str(pick(raw, 'name'))
	if (!name) return undefined
	const t = str(pick(raw, 'type'), 'string').toLowerCase()
	const type = t === 'bool' || t === 'int' || t === 'double' ? t : 'string'
	const description = pick(raw, 'description')
	return {
		name,
		type,
		required: pick(raw, 'required') === true,
		...(typeof description === 'string' ? { description } : {}),
	}
}

function normalizeAction(raw: unknown): ManifestAction | undefined {
	if (!isObj(raw)) return undefined
	const id = str(pick(raw, 'id'))
	if (!id) return undefined
	const rawParams = pick(raw, 'params')
	const params = (Array.isArray(rawParams) ? rawParams : [])
		.map(normalizeParam)
		.filter((p): p is ManifestParam => p !== undefined)
	return {
		id,
		title: str(pick(raw, 'title'), id),
		description: str(pick(raw, 'description')),
		oscAddress: str(pick(raw, 'oscAddress')),
		params,
	}
}

/** Tolerant manifest reader: accepts PascalCase (served today) and camelCase keys. */
export function normalizeManifest(raw: unknown): Manifest {
	const o = isObj(raw) ? raw : {}
	const rawActions = pick(o, 'actions')
	const rawFields = pick(o, 'feedbackFields')
	const version = pick(o, 'version')
	return {
		version: typeof version === 'number' ? version : 0,
		root: str(pick(o, 'root')),
		actions: (Array.isArray(rawActions) ? rawActions : [])
			.map(normalizeAction)
			.filter((a): a is ManifestAction => a !== undefined),
		feedbackFields: (Array.isArray(rawFields) ? rawFields : []).filter((f): f is string => typeof f === 'string'),
	}
}

export async function fetchManifest(
	baseUrl: string,
	headers: Record<string, string>,
	fetchImpl: typeof fetch = fetch,
): Promise<Manifest> {
	const res = await fetchImpl(`${baseUrl}/manifest`, { headers })
	if (!res.ok) throw new Error(`HTTP ${res.status}`)
	return normalizeManifest(await res.json())
}

/** POST /invoke exactly as the module does. Returns the response (callers log non-2xx). */
export async function invokeAction(
	baseUrl: string,
	headers: Record<string, string>,
	action: string,
	args: unknown[],
	fetchImpl: typeof fetch = fetch,
): Promise<Response> {
	return fetchImpl(`${baseUrl}/invoke`, {
		method: 'POST',
		headers,
		body: JSON.stringify({ action, args }),
	})
}
