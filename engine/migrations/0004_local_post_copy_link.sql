-- A copy of a local post remembers the post it was copied from (D-098), so
-- that a correction of a post can be found from the post it corrects. The
-- link is not content: nothing approves, sends or removes by it. Deleting
-- the post that was copied (a draft or a rejected one, D-094) leaves its
-- copies without a source.
ALTER TABLE local_posts ADD COLUMN copied_from_id bigint REFERENCES local_posts (id) ON DELETE SET NULL;
CREATE INDEX local_posts_copied_from_idx ON local_posts (copied_from_id) WHERE copied_from_id IS NOT NULL;
