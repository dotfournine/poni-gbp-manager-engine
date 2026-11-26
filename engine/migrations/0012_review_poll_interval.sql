-- How often reviews are polled is an engine setting, PONI_REVIEW_POLL_INTERVAL_S (D-112).
ALTER TABLE locations DROP COLUMN review_poll_interval_s;
