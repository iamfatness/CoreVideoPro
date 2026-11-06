# Credential-safe outputs drill errors — #1165

Problem: a refused join throws complete request JSON and arbitrary response text into persistent QA evidence. Either can contain a meeting password.

Specification: failed control calls retain only a validated action identifier and HTTP status (or transport failure). Never log request arguments, raw error messages, response bodies or nested exception text. Do not change request payloads or success results. Background launch uses a hidden window.

Plan: replace the error formatter with an allowlisted summary; exercise the actual catch path using a mocked failing HTTP request containing a synthetic password in both request and exception; cover HTTP and transport failures, unexpected action text and successful responses. Run existing offline drill tests. Use this harness revision against the frozen candidate; record both identities separately.

Acceptance: synthetic secret and URL are absent from caught/loggable failure output, action/status remain useful, and successful data is unchanged. No real meeting credentials enter test fixtures. Full output qualification remains independent.
