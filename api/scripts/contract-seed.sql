-- Rows for the contract check (check-contract.mjs) in a database that has
-- never synced with Google: one client, one location, two reviews. Made-up
-- data; safe to apply more than once. engine/scripts/linux.sh applies it to
-- the poni_linux database, never to a database with real data.
INSERT INTO clients (name, notes, created_at, updated_at)
VALUES ('Contract Check Spa', 'Seeded for the contract check', now(), now())
ON CONFLICT DO NOTHING;

INSERT INTO locations (gbp_location_id, gbp_account_id, client_id, title, primary_category, address, locality,
                       administrative_area, postal_code, region_code, phone, website_uri, maps_uri, raw,
                       first_seen_at, last_seen_at, updated_at, reviews_polled_at, reviews_full_synced_at,
                       last_sync_error, last_sync_error_at)
SELECT 'contract-location-1', 'contract-account-1', c.id, 'Contract Check Spa - Downtown', 'Spa',
       '1 Example Street', 'New York', 'NY', '10001', 'US', '(212) 555-0100', 'https://example.invalid/',
       'https://maps.example.invalid/1', '{"name": "locations/contract-location-1"}'::jsonb,
       now(), now(), now(), now(), now(), 'HTTP 503 (seeded example)', now()
  FROM clients c
 WHERE c.name = 'Contract Check Spa'
ON CONFLICT (gbp_location_id) DO NOTHING;

-- The location's reply settings (D-093): what linking gives the first
-- location of a client, a copy of the knowledge base's defaults. The
-- knowledge base is seeded by the time this file is applied.
INSERT INTO reply_settings (location_id, voice, localization, policy, created_at, updated_at)
SELECT l.id, d.voice, d.localization, d.policy, now(), now()
  FROM locations l, kb_defaults d
 WHERE l.gbp_location_id = 'contract-location-1'
ON CONFLICT (location_id) DO NOTHING;

INSERT INTO reviews (location_id, gbp_review_id, gbp_name, rating, comment, original_text, translated_text, language,
                     reviewer_name, reviewer_photo_url, reviewer_anonymous, created_time, updated_time, reply_text,
                     reply_updated_time, reply_state, reply_origin, removed_at, raw, first_seen_at, last_seen_at,
                     last_changed_at)
SELECT l.id, v.review_id, 'accounts/contract-account-1/locations/contract-location-1/reviews/' || v.review_id,
       v.rating, v.comment, v.original_text, v.translated_text, v.language, v.reviewer, NULL, false,
       now() - interval '2 days', now() - interval '1 day', v.reply, CASE WHEN v.reply IS NULL THEN NULL ELSE now() END,
       CASE WHEN v.reply IS NULL THEN NULL ELSE 'APPROVED' END, CASE WHEN v.reply IS NULL THEN NULL ELSE 'external' END,
       CASE WHEN v.removed THEN now() END, '{}'::jsonb, now(), now(), now()
  FROM locations l,
       (VALUES ('contract-review-1', 5,
                E'(Translated by Google) Very comfortable\n\n(Original)\n很舒服', '很舒服', 'Very comfortable', 'zh',
                'Reviewer One', 'Thank you!', false),
               ('contract-review-2', 2, NULL, NULL, NULL, NULL, 'Reviewer Two', NULL, true))
           AS v(review_id, rating, comment, original_text, translated_text, language, reviewer, reply, removed)
 WHERE l.gbp_location_id = 'contract-location-1'
ON CONFLICT (location_id, gbp_review_id) DO NOTHING;

INSERT INTO review_events (review_id, kind, source, occurred_at, detail)
SELECT r.id, 'discovered', 'full_sync', now(), '{"rating": 5, "has_text": true, "has_reply": true}'::jsonb
  FROM reviews r
 WHERE r.gbp_review_id = 'contract-review-1'
   AND NOT EXISTS (SELECT 1 FROM review_events e WHERE e.review_id = r.id);

-- Phase 2: the client's industry, an analysis of both reviews, a draft for
-- the one without a reply, and one model call, so the check has every shape
-- to validate without an LLM.
UPDATE clients SET industry_key = 'wellness' WHERE name = 'Contract Check Spa' AND industry_key IS NULL;

INSERT INTO review_analyses (review_id, review_updated_time, status, language, script, sentiment, contains_complaint,
                             topics, items, severity_flags, manipulation_suspected, manipulation_evidence, summary_en, industry_key, model, prompt_version, raw, created_at)
SELECT r.id, r.updated_time, 'ok', v.language, v.script, v.sentiment, v.complaint, v.topics::jsonb, v.items::jsonb,
       v.flags::jsonb, false, '', v.summary, 'wellness', v.model, 'seeded', NULL, now()
  FROM reviews r
  JOIN (VALUES ('contract-review-1', 'zh', 'Hans', 'positive', false,
                '[{"id": "relaxation_results", "polarity": "positive"}]',
                '[{"name": "头疗", "type": "treatment", "polarity": "positive"}]', '[]',
                'The guest found the visit very comfortable.', 'seeded-model'),
               ('contract-review-2', 'none', 'none', 'none', false, '[]', '[]',
                '[{"id": "injury", "label": "Injury", "sources": ["llm"], "evidence": "seeded example"}]',
                '', NULL))
           AS v(review_id, language, script, sentiment, complaint, topics, items, flags, summary, model)
    ON v.review_id = r.gbp_review_id
 WHERE NOT EXISTS (SELECT 1 FROM review_analyses a WHERE a.review_id = r.id);

INSERT INTO reply_drafts (review_id, analysis_id, review_updated_time, status, status_reason, route, route_reasons, mode,
                          text, language, script, generation_ok,
                          generator_manipulation_suspected, notes_for_staff, checks, model, prompt_version, inputs,
                          generated_text, created_at, updated_at)
SELECT r.id, a.id, r.updated_time, 'cancelled', 'the review was removed', 'escalate', '["severity: injury"]'::jsonb,
       'severity', 'Thank you for letting us know. Please reach out to us directly.', 'en', NULL, true, false,
       'Seeded example.', '[{"name": "not_empty", "passed": true, "detail": ""},
                            {"name": "length", "passed": false, "detail": "63 characters, expected 120 to 650"}]'::jsonb,
       'seeded-model', 'seeded', '{"mode": "severity", "target_language": "en"}'::jsonb,
       'Thank you for letting us know. Please reach out to us directly.', now(), now()
  FROM reviews r JOIN review_analyses a ON a.review_id = r.id
 WHERE r.gbp_review_id = 'contract-review-2'
   AND NOT EXISTS (SELECT 1 FROM reply_drafts d WHERE d.review_id = r.id);

-- Phase 3: an escalation for the flagged review, so its endpoints have something to answer with.
INSERT INTO cases (review_id, opened_by, analysis_id, severity_flags, opened_at, updated_at)
SELECT r.id, 'severity_flag', a.id, a.severity_flags, now(), now()
  FROM reviews r JOIN review_analyses a ON a.review_id = r.id
 WHERE r.gbp_review_id = 'contract-review-2'
    ON CONFLICT (review_id) DO NOTHING;

INSERT INTO llm_calls (occurred_at, task, model, client_id, location_id, review_id, attempt, status, error, http_status,
                       prompt_tokens, completion_tokens, latency_ms, estimated_cost_usd, finish_reason)
SELECT now(), v.task, 'seeded-model', l.client_id, l.id, r.id, v.attempt, v.status, v.error, v.http_status, v.prompt_tokens,
       v.completion_tokens, 3200, v.cost, v.finish
  FROM reviews r JOIN locations l ON l.id = r.location_id,
       (VALUES ('analyzer', 1, 'error', 'unavailable: HTTP 503', 503, 0, 0, NULL::numeric, NULL),
               ('analyzer', 2, 'ok', NULL, 200, 1400, 90, 0.000704, 'stop'))
           AS v(task, attempt, status, error, http_status, prompt_tokens, completion_tokens, cost, finish)
 WHERE r.gbp_review_id = 'contract-review-1'
   AND NOT EXISTS (SELECT 1 FROM llm_calls k WHERE k.model = 'seeded-model');

-- Phase 4.1: a finished go-live test with two cases, so its endpoints have
-- something to answer with. A made-up fingerprint: it gives nothing a pass,
-- and no configuration on GET /api/go-live shows it, so it has a fixed id
-- that check-contract.mjs reads it by.
INSERT INTO go_live_runs (id, client_id, fingerprint, fingerprint_inputs, location_ids, status, summary, auto_post_positive,
                          auto_post_rating_only, auto_post_min_stars, corpus_version, repeats, real_reviews, started_at, finished_at)
OVERRIDING SYSTEM VALUE
SELECT 900000001, c.id, 'contract-seeded', '{}'::jsonb, (SELECT coalesce(jsonb_agg(l.id), '[]'::jsonb) FROM locations l WHERE l.client_id = c.id),
       'failed', '1 of 2 cases failed: 1 reply that would have been posted the judge rated unacceptable.', true, true, 4,
       'seeded', 3, 1, now() - interval '20 minutes', now() - interval '10 minutes'
  FROM clients c
 WHERE c.name = 'Contract Check Spa'
   AND NOT EXISTS (SELECT 1 FROM go_live_runs g WHERE g.id = 900000001);  -- also where an older seed left one under another id

INSERT INTO go_live_cases (run_id, case_key, category, review_id, stars, queued, verdict, detail, attempts, finished_at)
SELECT g.id, v.case_key, v.category, CASE WHEN v.category = 'real' THEN r.id END, v.stars, true, v.verdict, v.detail, v.attempts::jsonb, now()
  FROM go_live_runs g, reviews r,
       (VALUES ('M-01', 'manipulation', 5, 'ok', NULL,
                '[{"route": "approval_queue", "reasons": ["manipulation_suspected"], "reply": "Thank you for your kind words!", "failed_checks": [], "rating": null, "rating_reason": null, "rated_by": null}]'),
               ('review:seeded', 'real', 5, 'unacceptable', 'The reply promises a discount.',
                '[{"route": "auto_post", "reasons": [], "reply": "Thank you! Enjoy 10% off next time.", "failed_checks": [], "rating": "unacceptable", "rating_reason": "The reply promises a discount.", "rated_by": null}]'))
           AS v(case_key, category, stars, verdict, detail, attempts)
 WHERE g.id = 900000001 AND r.gbp_review_id = 'contract-review-1'
ON CONFLICT (run_id, case_key) DO NOTHING;

-- Phase 6: two local posts, so the list and the post's page answer with every
-- part of a post: a draft of an offer written here, and a post made at
-- Google with a button and two photos.
INSERT INTO local_posts (location_id, origin, status, content, checks, created_at, updated_at)
SELECT l.id, 'system', 'draft',
       jsonb_build_object(
           'topic_type', 'offer', 'summary', 'Seeded for the contract check: ten percent off this week.', 'language_code', 'en',
           'call_to_action', NULL,
           'event', jsonb_build_object(
               'title', 'Opening week',
               'schedule', jsonb_build_object('start_date', to_char(current_date + 30, 'YYYY-MM-DD'), 'start_time', '10:00',
                                              'end_date', to_char(current_date + 37, 'YYYY-MM-DD'), 'end_time', '20:00'),
               'recurrence', jsonb_build_object(
                   'frequency', 'weekly',
                   'until', to_char((now() + interval '90 days') AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.US"Z"'))),
           'offer', jsonb_build_object('coupon_code', 'SEEDED', 'redeem_online_url', NULL, 'terms_conditions', 'Made-up terms.'),
           'media_ids', '[]'::jsonb,
           'scheduled_time', to_char((now() + interval '29 days') AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.US"Z"')),
       '[{"name": "summary_length", "passed": true, "detail": ""}]'::jsonb, now(), now()
  FROM locations l
 WHERE l.gbp_location_id = 'contract-location-1'
   AND NOT EXISTS (SELECT 1 FROM local_posts p WHERE p.location_id = l.id AND p.origin = 'system');

INSERT INTO local_posts (location_id, origin, status, content,
                         google_name, google_state, search_url, google_media, google_created_at, google_updated_at,
                         raw, synced_at, created_at, updated_at)
SELECT l.id, 'google', 'sent',
       '{"topic_type": "standard", "summary": "Seeded for the contract check: a post made at Google.", "language_code": "en",
         "call_to_action": {"action_type": "learn_more", "url": "https://example.invalid/news"},
         "event": null, "offer": null, "media_ids": [], "scheduled_time": null}'::jsonb,
       'accounts/contract-account-1/locations/contract-location-1/localPosts/contract-post-1',
       'live', 'https://local.example.invalid/posts/1',
       '[{"format": "photo", "url": "https://media.example.invalid/1.jpg"},
         {"format": "photo", "url": "https://media.example.invalid/2.jpg"}]'::jsonb,
       now() - interval '3 days', now() - interval '3 days', '{}'::jsonb, now(), now(), now()
  FROM locations l
 WHERE l.gbp_location_id = 'contract-location-1'
ON CONFLICT (google_name) DO NOTHING;
