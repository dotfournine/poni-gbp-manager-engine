-- "Auto reply" is a reply the engine posts without a person (D-110): the
-- columns, constraints and route value of the replies it posts take that
-- name. Nothing but names changes.

ALTER TABLE locations RENAME COLUMN auto_post_enabled TO auto_reply_enabled;
ALTER TABLE locations RENAME COLUMN auto_post_five_star TO auto_reply_five_star;
ALTER TABLE locations RENAME COLUMN auto_post_four_star TO auto_reply_four_star;
ALTER TABLE locations RENAME COLUMN auto_post_daily_cap TO auto_reply_daily_cap;
ALTER TABLE locations RENAME COLUMN auto_post_paused_at TO auto_reply_paused_at;
ALTER TABLE locations RENAME COLUMN auto_post_pause_reason TO auto_reply_pause_reason;
ALTER TABLE locations RENAME COLUMN auto_post_pause_detail TO auto_reply_pause_detail;
ALTER TABLE locations RENAME COLUMN auto_post_resumed_at TO auto_reply_resumed_at;
ALTER TABLE locations RENAME COLUMN auto_post_pause_after_failures TO auto_reply_pause_after_failures;
ALTER TABLE locations RENAME CONSTRAINT locations_auto_post_pause_check TO locations_auto_reply_pause_check;
ALTER TABLE locations RENAME CONSTRAINT locations_auto_post_daily_cap_check TO locations_auto_reply_daily_cap_check;
ALTER TABLE locations RENAME CONSTRAINT locations_auto_post_pause_reason_check TO locations_auto_reply_pause_reason_check;
ALTER TABLE locations RENAME CONSTRAINT locations_auto_post_pause_after_failures_check TO locations_auto_reply_pause_after_failures_check;

ALTER TABLE system_settings RENAME COLUMN auto_post_paused_at TO auto_reply_paused_at;
ALTER TABLE system_settings RENAME COLUMN auto_post_pause_reason TO auto_reply_pause_reason;
ALTER TABLE system_settings RENAME COLUMN auto_post_pause_detail TO auto_reply_pause_detail;
ALTER TABLE system_settings RENAME COLUMN auto_post_resumed_at TO auto_reply_resumed_at;
ALTER TABLE system_settings RENAME CONSTRAINT system_settings_auto_post_pause_check TO system_settings_auto_reply_pause_check;
ALTER TABLE system_settings RENAME CONSTRAINT system_settings_auto_post_pause_reason_check TO system_settings_auto_reply_pause_reason_check;

ALTER TABLE go_live_runs RENAME COLUMN auto_post_positive TO auto_reply_positive;
ALTER TABLE go_live_runs RENAME COLUMN auto_post_rating_only TO auto_reply_rating_only;
ALTER TABLE go_live_runs RENAME COLUMN auto_post_min_stars TO auto_reply_min_stars;

-- The route value: renamed in the rows too, so that the new CHECK holds on a
-- database that has drafts.
ALTER TABLE reply_drafts DROP CONSTRAINT reply_drafts_route_check;
UPDATE reply_drafts SET route = 'auto_reply' WHERE route = 'auto_post';
ALTER TABLE reply_drafts ADD CONSTRAINT reply_drafts_route_check CHECK (route IN ('auto_reply', 'approval_queue', 'escalate'));
