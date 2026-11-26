-- A notification rule's recipients become a native array (D-107: a list of
-- plain values is a text[], never jsonb, and holds no NULL). The rows are
-- converted in place: each rule keeps the addresses its jsonb list held, in
-- the same order. PostgreSQL takes no subquery in a column's USING
-- expression, so the conversion is a function of this session only.

CREATE FUNCTION pg_temp.recipients_array(list jsonb) RETURNS text[] LANGUAGE sql IMMUTABLE AS $$
    SELECT ARRAY(SELECT e FROM jsonb_array_elements_text(list) WITH ORDINALITY AS t(e, ord) ORDER BY ord)
$$;

ALTER TABLE notification_rules ALTER COLUMN recipients DROP DEFAULT;
ALTER TABLE notification_rules ALTER COLUMN recipients TYPE text[] USING pg_temp.recipients_array(recipients);
ALTER TABLE notification_rules ALTER COLUMN recipients SET DEFAULT '{}';
ALTER TABLE notification_rules ADD CONSTRAINT notification_rules_recipients_check
    CHECK (array_position(recipients, NULL) IS NULL AND cardinality(recipients) <= 20);

DROP FUNCTION pg_temp.recipients_array(jsonb);
