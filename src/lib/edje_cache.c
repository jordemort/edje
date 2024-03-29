#include "edje_private.h"


static Eina_Hash   *_edje_file_hash = NULL;
static int          _edje_file_cache_size = 16;
static Eina_List   *_edje_file_cache = NULL;

static int          _edje_collection_cache_size = 16;

static Edje_Part_Collection *
_edje_file_coll_open(Edje_File *edf, const char *coll)
{
   Edje_Part_Collection *edc = NULL;
   Edje_Part_Collection_Directory_Entry *ce;
   int id = -1, size = 0;
   Eina_List *l;
   char buf[256];
   char *buffer;
   void *data;

   ce = eina_hash_find(edf->collection, coll);
   if (!ce) return NULL;

   if (ce->ref)
     {
	ce->ref->references++;
	return ce->ref;
     }

   EINA_LIST_FOREACH(edf->collection_cache, l, edc)
     {
	if (!strcmp(edc->part, coll))
	  {
	     edc->references = 1;
	     ce->ref = edc;

	     edf->collection_cache = eina_list_remove_list(edf->collection_cache, l);
	     return ce->ref;
	  }
     }

   id = ce->id;
   if (id < 0) return NULL;

#define INIT_EMP(Tp, Sz, Ce)                                           \
   buffer = alloca(strlen(ce->entry) + strlen(#Tp) + 2);               \
   sprintf(buffer, "%s/%s", ce->entry, #Tp);                           \
   Ce->mp.Tp = eina_mempool_add("one_big", buffer, NULL, sizeof (Sz), Ce->count.Tp); \
   _emp_##Tp = Ce->mp.Tp;

#define INIT_EMP_BOTH(Tp, Sz, Ce)                                       \
   INIT_EMP(Tp, Sz, Ce)                                                 \
   Ce->mp_rtl.Tp = eina_mempool_add("one_big", buffer, NULL,            \
         sizeof (Sz), Ce->count.Tp);

   INIT_EMP_BOTH(RECTANGLE, Edje_Part_Description_Common, ce);
   INIT_EMP_BOTH(TEXT, Edje_Part_Description_Text, ce);
   INIT_EMP_BOTH(IMAGE, Edje_Part_Description_Image, ce);
   INIT_EMP_BOTH(PROXY, Edje_Part_Description_Proxy, ce);
   INIT_EMP_BOTH(SWALLOW, Edje_Part_Description_Common, ce);
   INIT_EMP_BOTH(TEXTBLOCK, Edje_Part_Description_Text, ce);
   INIT_EMP_BOTH(GROUP, Edje_Part_Description_Common, ce);
   INIT_EMP_BOTH(BOX, Edje_Part_Description_Box, ce);
   INIT_EMP_BOTH(TABLE, Edje_Part_Description_Table, ce);
   INIT_EMP_BOTH(EXTERNAL, Edje_Part_Description_External, ce);
   INIT_EMP_BOTH(SPACER, Edje_Part_Description_Common, ce);
   INIT_EMP(part, Edje_Part, ce);

   snprintf(buf, sizeof(buf), "edje/collections/%i", id);
   edc = eet_data_read(edf->ef, _edje_edd_edje_part_collection, buf);
   if (!edc) return NULL;

   edc->references = 1;
   edc->part = ce->entry;

   /* For Edje file build with Edje 1.0 */
   if (edf->version <= 3 && edf->minor <= 1)
     {
        /* This will preserve previous rendering */
        unsigned int i;

	/* people expect signal to not be broadcasted */
	edc->broadcast_signal = EINA_FALSE;

	/* people expect text.align to be 0.0 0.0 */
        for (i = 0; i < edc->parts_count; ++i)
          {
             if (edc->parts[i]->type == EDJE_PART_TYPE_TEXTBLOCK)
               {
                  Edje_Part_Description_Text *text;
                  unsigned int j;

                  text = (Edje_Part_Description_Text*) edc->parts[i]->default_desc;
                  text->text.align.x = TO_DOUBLE(0.0);
                  text->text.align.y = TO_DOUBLE(0.0);

                  for (j = 0; j < edc->parts[i]->other.desc_count; ++j)
                    {
                       text =  (Edje_Part_Description_Text*) edc->parts[i]->other.desc[j];
                       text->text.align.x = TO_DOUBLE(0.0);
                       text->text.align.y = TO_DOUBLE(0.0);
                    }
               }
          }
     }

   snprintf(buf, sizeof(buf), "edje/scripts/embryo/compiled/%i", id);
   data = eet_read(edf->ef, buf, &size);

   if (data)
     {
	edc->script = embryo_program_new(data, size);
	_edje_embryo_script_init(edc);
	free(data);
     }

   snprintf(buf, sizeof(buf), "edje/scripts/lua/%i", id);
   data = eet_read(edf->ef, buf, &size);

   if (data)
     {
        _edje_lua2_script_load(edc, data, size);
	free(data);
     }

   ce->ref = edc;

   return edc;
}

#ifdef HAVE_EIO
static Eina_Bool
_edje_file_warn(void *data)
{
   Edje_File *edf = data;
   Eina_List *l, *ll;
   Edje *ed;

   edf->references++;

   EINA_LIST_FOREACH(edf->edjes, l, ed)
     _edje_ref(ed);

   EINA_LIST_FOREACH(edf->edjes, l, ed)
     {
        _edje_emit(ed, "edje,change,file", "edje");
     }

   EINA_LIST_FOREACH_SAFE(edf->edjes, l, ll, ed)
     _edje_unref(ed);

   edf->references--;

   edf->timeout = NULL;
   return EINA_FALSE;
}

static Eina_Bool
_edje_file_change(void *data, int ev_type __UNUSED__, void *event)
{
   Edje_File *edf = data;
   Eio_Monitor_Event *ev = event;

   if (ev->monitor == edf->monitor)
     {
        if (edf->timeout) ecore_timer_del(edf->timeout);
        edf->timeout = ecore_timer_add(0.5, _edje_file_warn, edf);
     }
   return ECORE_CALLBACK_PASS_ON;
}
#endif

static Edje_File *
_edje_file_open(const char *file, const char *coll, int *error_ret, Edje_Part_Collection **edc_ret, time_t mtime)
{
   Edje_Color_Class *cc;
   Edje_File *edf;
   Eina_List *l;
   Edje_Part_Collection *edc;
   Eet_File *ef;
#ifdef HAVE_EIO
   Ecore_Event_Handler *ev;
#endif

   ef = eet_open(file, EET_FILE_MODE_READ);
   if (!ef)
     {
	*error_ret = EDJE_LOAD_ERROR_UNKNOWN_FORMAT;
	return NULL;
     }
   edf = eet_data_read(ef, _edje_edd_edje_file, "edje/file");
   if (!edf)
     {
	*error_ret = EDJE_LOAD_ERROR_CORRUPT_FILE;
	eet_close(ef);
	return NULL;
     }

   edf->ef = ef;
   edf->mtime = mtime;
#ifdef HAVE_EIO
   edf->monitor = eio_monitor_add(file);
   ev = ecore_event_handler_add(EIO_MONITOR_FILE_DELETED, _edje_file_change, edf);
   edf->handlers = eina_list_append(edf->handlers, ev);
   ev = ecore_event_handler_add(EIO_MONITOR_FILE_MODIFIED, _edje_file_change, edf);
   edf->handlers = eina_list_append(edf->handlers, ev);
   ev = ecore_event_handler_add(EIO_MONITOR_FILE_CREATED, _edje_file_change, edf);
   edf->handlers = eina_list_append(edf->handlers, ev);
   ev = ecore_event_handler_add(EIO_MONITOR_SELF_DELETED, _edje_file_change, edf);
   edf->handlers = eina_list_append(edf->handlers, ev);
#endif

   if (edf->version != EDJE_FILE_VERSION)
     {
	*error_ret = EDJE_LOAD_ERROR_INCOMPATIBLE_FILE;
	_edje_file_free(edf);
	return NULL;
     }
   if (!edf->collection)
     {
	*error_ret = EDJE_LOAD_ERROR_CORRUPT_FILE;
	_edje_file_free(edf);
	return NULL;
     }

   if (edf->minor > EDJE_FILE_MINOR)
     {
	WRN("`%s` may use feature from a newer edje and could not show up as expected.", file);
     }

   edf->path = eina_stringshare_add(file);
   edf->references = 1;

   /* This should be done at edje generation time */
   _edje_textblock_style_parse_and_fix(edf);
   edf->color_hash = eina_hash_string_small_new(NULL);
   EINA_LIST_FOREACH(edf->color_classes, l, cc)
     if (cc->name)
       eina_hash_direct_add(edf->color_hash, cc->name, cc);

   if (coll)
     {
	edc = _edje_file_coll_open(edf, coll);
	if (!edc)
	  {
	     *error_ret = EDJE_LOAD_ERROR_UNKNOWN_COLLECTION;
	  }
	if (edc_ret) *edc_ret = edc;
     }

   return edf;
}

static void
_edje_file_dangling(Edje_File *edf)
{
   if (edf->dangling) return;
   edf->dangling = EINA_TRUE;

   eina_hash_del(_edje_file_hash, edf->path, edf);
   if (!eina_hash_population(_edje_file_hash))
     {
       eina_hash_free(_edje_file_hash);
       _edje_file_hash = NULL;
     }
}

Edje_File *
_edje_cache_file_coll_open(const char *file, const char *coll, int *error_ret, Edje_Part_Collection **edc_ret, Edje *ed)
{
   Edje_File *edf;
   Eina_List *l, *hist;
   Edje_Part_Collection *edc;
   Edje_Part *ep;
   struct stat st;

   if (stat(file, &st) != 0)
     {
        *error_ret = EDJE_LOAD_ERROR_DOES_NOT_EXIST;
        return NULL;
     }

   if (!_edje_file_hash)
     {
	_edje_file_hash = eina_hash_string_small_new(NULL);
	goto find_list;
     }

   edf = eina_hash_find(_edje_file_hash, file);
   if (edf)
     {
	if (edf->mtime != st.st_mtime)
	  {
	     _edje_file_dangling(edf);
	     goto open_new;
	  }

	edf->references++;
	goto open;
     }
   
find_list:
   EINA_LIST_FOREACH(_edje_file_cache, l, edf)
     {
	if (!strcmp(edf->path, file))
	  {
	     if (edf->mtime != st.st_mtime)
	       {
		  _edje_file_cache = eina_list_remove_list(_edje_file_cache, l);
		  _edje_file_free(edf);
		  goto open_new;
	       }

	     edf->references = 1;
	     _edje_file_cache = eina_list_remove_list(_edje_file_cache, l);
	     eina_hash_add(_edje_file_hash, file, edf);
	     goto open;
	  }
     }

open_new:
   if (!_edje_file_hash)
      _edje_file_hash = eina_hash_string_small_new(NULL);

   edf = _edje_file_open(file, coll, error_ret, edc_ret, st.st_mtime);
   if (!edf)
      return NULL;

#ifdef HAVE_EIO
   if (ed) edf->edjes = eina_list_append(edf->edjes, ed);
#else
   (void) ed;
#endif

   eina_hash_add(_edje_file_hash, file, edf);
   return edf;

open:
   if (!coll)
      return edf;

   edc = _edje_file_coll_open(edf, coll);
   if (!edc)
     {
	*error_ret = EDJE_LOAD_ERROR_UNKNOWN_COLLECTION;
     }
   else
     {
	if (!edc->checked)
	  {
	     unsigned int j;

	     for (j = 0; j < edc->parts_count; ++j)
	       {
		  Edje_Part *ep2;

		  ep = edc->parts[j];

		  /* Register any color classes in this parts descriptions. */
		  hist = NULL;
		  hist = eina_list_append(hist, ep);
		  ep2 = ep;
		  while (ep2->dragable.confine_id >= 0)
		    {
		       if (ep2->dragable.confine_id >= (int) edc->parts_count)
			 {
			    ERR("confine_to above limit. invalidating it.");
			    ep2->dragable.confine_id = -1;
			    break;
			 }

		       ep2 = edc->parts[ep2->dragable.confine_id];
		       if (eina_list_data_find(hist, ep2))
			 {
			    ERR("confine_to loops. invalidating loop.");
			    ep2->dragable.confine_id = -1;
			    break;
			 }
		       hist = eina_list_append(hist, ep2);
		    }
		  eina_list_free(hist);
		  hist = NULL;
		  hist = eina_list_append(hist, ep);
		  ep2 = ep;
		  while (ep2->dragable.event_id >= 0)
		    {
		       Edje_Part* prev;

		       if (ep2->dragable.event_id >= (int) edc->parts_count)
			 {
			    ERR("event_id above limit. invalidating it.");
			    ep2->dragable.event_id = -1;
			    break;
			 }
		       prev = ep2;

		       ep2 = edc->parts[ep2->dragable.event_id];
		       if (!ep2->dragable.x && !ep2->dragable.y)
			 {
			    prev->dragable.event_id = -1;
			    break;
			 }

		       if (eina_list_data_find(hist, ep2))
			 {
			    ERR("events_to loops. invalidating loop.");
			    ep2->dragable.event_id = -1;
			    break;
			 }
		       hist = eina_list_append(hist, ep2);
		    }
		  eina_list_free(hist);
		  hist = NULL;
		  hist = eina_list_append(hist, ep);
		  ep2 = ep;
		  while (ep2->clip_to_id >= 0)
		    {
		       if (ep2->clip_to_id >= (int) edc->parts_count)
			 {
			    ERR("clip_to_id above limit. invalidating it.");
			    ep2->clip_to_id = -1;
			    break;
			 }

		       ep2 = edc->parts[ep2->clip_to_id];
		       if (eina_list_data_find(hist, ep2))
			 {
			    ERR("clip_to loops. invalidating loop.");
			    ep2->clip_to_id = -1;
			    break;
			 }
		       hist = eina_list_append(hist, ep2);
		    }
		  eina_list_free(hist);
		  hist = NULL;
	       }
	    edc->checked = 1;
	  }
     }
#ifdef HAVE_EIO
   if (edc && ed) edf->edjes = eina_list_append(edf->edjes, ed);
#else
   (void) ed;
#endif

   if (edc_ret) *edc_ret = edc;

   return edf;
}

void
_edje_cache_coll_clean(Edje_File *edf)
{
   while ((edf->collection_cache) &&
	  (eina_list_count(edf->collection_cache) > (unsigned int) _edje_collection_cache_size))
     {
	Edje_Part_Collection_Directory_Entry *ce;
	Edje_Part_Collection *edc;

	edc = eina_list_data_get(eina_list_last(edf->collection_cache));
	edf->collection_cache = eina_list_remove_list(edf->collection_cache, eina_list_last(edf->collection_cache));

	ce = eina_hash_find(edf->collection, edc->part);
	_edje_collection_free(edf, edc, ce);
     }
}

void
_edje_cache_coll_flush(Edje_File *edf)
{
   while (edf->collection_cache)
     {
	Edje_Part_Collection_Directory_Entry *ce;
	Edje_Part_Collection *edc;
	Eina_List *last;

	last = eina_list_last(edf->collection_cache);
	edc = eina_list_data_get(last);
	edf->collection_cache = eina_list_remove_list(edf->collection_cache,
						      last);

	ce = eina_hash_find(edf->collection, edc->part);
	_edje_collection_free(edf, edc, ce);
     }
}

void
_edje_cache_coll_unref(Edje_File *edf, Edje_Part_Collection *edc)
{
   Edje_Part_Collection_Directory_Entry *ce;

   edc->references--;
   if (edc->references != 0) return;

   ce = eina_hash_find(edf->collection, edc->part);
   if (!ce)
     {
	ERR("Something is wrong with reference count of '%s'.", edc->part);
     }
   else if (ce->ref)
     {
	ce->ref = NULL;

	if (edf->dangling)
	  {
	     /* No need to keep the collection around if the file is dangling */
	     _edje_collection_free(edf, edc, ce);
	     _edje_cache_coll_flush(edf);
	  }
	else
	  {
	     edf->collection_cache = eina_list_prepend(edf->collection_cache, edc);
	     _edje_cache_coll_clean(edf);
	  }
     }
}

static void
_edje_cache_file_clean(void)
{
   int count;

   count = eina_list_count(_edje_file_cache);
   while ((_edje_file_cache) && (count > _edje_file_cache_size))
     {
	Eina_List *last;
	Edje_File *edf;

	last = eina_list_last(_edje_file_cache);
	edf = eina_list_data_get(last);
	_edje_file_cache = eina_list_remove_list(_edje_file_cache, last);
	_edje_file_free(edf);
	count = eina_list_count(_edje_file_cache);
     }
}

void
_edje_cache_file_unref(Edje_File *edf)
{
   edf->references--;
   if (edf->references != 0) return;

   if (edf->dangling)
     {
	_edje_file_free(edf);
	return;
     }

   eina_hash_del(_edje_file_hash, edf->path, edf);
   if (!eina_hash_population(_edje_file_hash))
     {
       eina_hash_free(_edje_file_hash);
       _edje_file_hash = NULL;
     }
   _edje_file_cache = eina_list_prepend(_edje_file_cache, edf);
   _edje_cache_file_clean();
}

void
_edje_file_cache_shutdown(void)
{
   edje_file_cache_flush();
}


/*============================================================================*
 *                                 Global                                     *
 *============================================================================*/

/*============================================================================*
 *                                   API                                      *
 *============================================================================*/


EAPI void
edje_file_cache_set(int count)
{
   if (count < 0) count = 0;
   _edje_file_cache_size = count;
   _edje_cache_file_clean();
}


EAPI int
edje_file_cache_get(void)
{
   return _edje_file_cache_size;
}


EAPI void
edje_file_cache_flush(void)
{
   int ps;

   ps = _edje_file_cache_size;
   _edje_file_cache_size = 0;
   _edje_cache_file_clean();
   _edje_file_cache_size = ps;
}


EAPI void
edje_collection_cache_set(int count)
{
   Eina_List *l;
   Edje_File *edf;

   if (count < 0) count = 0;
   _edje_collection_cache_size = count;
   EINA_LIST_FOREACH(_edje_file_cache, l, edf)
     _edje_cache_coll_clean(edf);
   /* FIXME: freach in file hash too! */
}


EAPI int
edje_collection_cache_get(void)
{
   return _edje_collection_cache_size;
}


EAPI void
edje_collection_cache_flush(void)
{
   int ps;
   Eina_List *l;
   Edje_File *edf;

   ps = _edje_collection_cache_size;
   _edje_collection_cache_size = 0;
   EINA_LIST_FOREACH(_edje_file_cache, l, edf)
     _edje_cache_coll_flush(edf);
   /* FIXME: freach in file hash too! */
   _edje_collection_cache_size = ps;
}
