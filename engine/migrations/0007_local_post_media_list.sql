-- A local post carries up to ten photos and videos (D-101): the content's
-- "media_id" (one upload or null) becomes "media_ids", the uploads in the
-- order the post shows them ([] for none). It is rewritten wherever the
-- engine keeps a content, because an approval and a claim compare contents
-- field for field: a post's content and approved content, and an edit's
-- content, base and approved content. Audit entries keep what they said.
--
-- The generated columns that held the one upload to a row of
-- local_post_media go: PostgreSQL has no foreign key on the elements of a
-- list. The engine checks that each id names an upload when a content is
-- saved, and nothing deletes an upload.

ALTER TABLE local_posts DROP COLUMN media_id;
ALTER TABLE local_post_edits DROP COLUMN media_id;

CREATE FUNCTION pg_temp.with_media_list(content jsonb) RETURNS jsonb LANGUAGE sql IMMUTABLE AS $$
    SELECT CASE WHEN content IS NULL THEN NULL
                ELSE (content - 'media_id') || jsonb_build_object('media_ids',
                         CASE WHEN jsonb_typeof(content->'media_id') = 'number' THEN jsonb_build_array(content->'media_id')
                              ELSE '[]'::jsonb END)
           END
$$;

UPDATE local_posts
   SET content = pg_temp.with_media_list(content),
       approved_content = pg_temp.with_media_list(approved_content);

UPDATE local_post_edits
   SET content = pg_temp.with_media_list(content),
       base_content = pg_temp.with_media_list(base_content),
       approved_content = pg_temp.with_media_list(approved_content);
