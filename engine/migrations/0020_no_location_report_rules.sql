-- A report's recipients are its location's schedules' (D-111 point 4), so no
-- notification rule is for `location_report` any more: the event type is
-- left only to the delivery log, as the type of a report's delivery rows,
-- and notification_deliveries keeps it.
--
-- After 0018 no such rule can exist: a `location_report` rule needs a
-- location (notification_rules_location_report_check), and 0018 refused any
-- database with one. The DELETE stays because it costs nothing and makes the
-- narrowed CHECK safe whatever is there.

DELETE FROM notification_rules WHERE event_type = 'location_report';

ALTER TABLE notification_rules DROP CONSTRAINT notification_rules_location_report_check;
ALTER TABLE notification_rules DROP CONSTRAINT notification_rules_event_type_check;
ALTER TABLE notification_rules ADD CONSTRAINT notification_rules_event_type_check
    CHECK (event_type IN ('escalation', 'approval_needed', 'system_problem'));
