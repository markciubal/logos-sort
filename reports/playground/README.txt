Playground paste kit.

1. state.txt              paste into the state box (plain text; Jev is text-only)
2. questions-starter.json three questions, one of each type, all with known answers:
                          Q_M_trace expects noul ~1.0, Q_SZ_networks expects level 4
                          (1.40-2.0), Q_MECH_lattice_idle expects thread_creation.
                          Run this first to confirm the shape before pasting the rest.
3. questions-01..10.json  ten at a time, each a bare map of id -> question

Every file's ROOT is the questions map itself. Nothing may sit beside the ids: the
validator reads any other top-level key as a question and rejects it.

Scoring: ../typesafe-answers-key.json, keyed by the same ids. Never paste that one -
it holds the answers, and the schema rejects unexpected properties anyway.
