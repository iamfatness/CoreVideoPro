import { describe, expect, it } from 'vitest';
import lifecycleFixtures from '../../../contracts/lifecycle.fixtures.json';
import identityFixtures from '../../../contracts/identity.fixtures.json';
import evidenceFixtures from '../../../contracts/evidence.fixtures.json';
const fixtures = [...lifecycleFixtures, ...identityFixtures, ...evidenceFixtures];
import * as browser from './lifecycle';
import * as node from '../../../native-core/src/generated/lifecycle';

describe('shared lifecycle wire fixtures', () => {
  for (const fixture of fixtures) it(fixture.id, () => {
    const payload: unknown = JSON.parse(fixture.json);
    const name = `validate${fixture.contract}` as keyof typeof browser;
    expect(browser[name](payload)).toBe(fixture.accepted);
    expect(node[name](payload)).toBe(fixture.accepted);
  });
});
