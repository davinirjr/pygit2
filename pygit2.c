/*
 * Copyright 2010 Google, Inc.
 *
 * This file is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2,
 * as published by the Free Software Foundation.
 *
 * In addition to the permissions in the GNU General Public License,
 * the authors give you unlimited permission to link the compiled
 * version of this file into combinations with other programs,
 * and to distribute those combinations without any restriction
 * coming from the use of this file.  (The General Public License
 * restrictions do apply in other respects; for example, they cover
 * modification of the file, and distribution when not linked into
 * a combined executable.)
 *
 * This file is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <Python.h>
#include <git/commit.h>
#include <git/common.h>
#include <git/repository.h>
#include <git/commit.h>
#include <git/odb.h>

typedef struct {
    PyObject_HEAD
    git_repository *repo;
} Repository;

/* The structs for the various object subtypes are identical except for the type
 * of their object pointers. */
#define OBJECT_STRUCT(_name, _ptr_type, _ptr_name) \
        typedef struct {\
            PyObject_HEAD\
            Repository *repo;\
            int own_obj:1;\
            _ptr_type *_ptr_name;\
        } _name;

OBJECT_STRUCT(Object, git_object, obj)
OBJECT_STRUCT(Commit, git_commit, commit)
OBJECT_STRUCT(Tree, git_tree, tree)
OBJECT_STRUCT(Blob, git_object, blob)

typedef struct {
    PyObject_HEAD
    git_tree_entry *entry;
    Tree *tree;
} TreeEntry;

static PyTypeObject RepositoryType;
static PyTypeObject ObjectType;
static PyTypeObject CommitType;
static PyTypeObject TreeEntryType;
static PyTypeObject TreeType;
static PyTypeObject BlobType;

static int
Repository_init(Repository *self, PyObject *args, PyObject *kwds) {
    char *path;

    if (kwds) {
        PyErr_SetString(PyExc_TypeError,
                        "Repository takes no keyword arugments");
        return -1;
    }

    if (!PyArg_ParseTuple(args, "s", &path))
        return -1;

    self->repo = git_repository_open(path);
    if (!self->repo) {
        PyErr_Format(PyExc_RuntimeError, "Failed to open repo directory at %s",
                     path);
        return -1;
    }

    return 0;
}

static void
Repository_dealloc(Repository *self) {
    if (self->repo)
        git_repository_free(self->repo);
    self->ob_type->tp_free((PyObject*)self);
}

static int
Repository_contains(Repository *self, PyObject *value) {
    char *hex;
    git_oid oid;

    hex = PyString_AsString(value);
    if (!hex)
        return -1;
    if (git_oid_mkstr(&oid, hex) < 0) {
        PyErr_Format(PyExc_ValueError, "Invalid hex SHA \"%s\"", hex);
        return -1;
    }
    return git_odb_exists(git_repository_database(self->repo), &oid);
}

static Object *wrap_object(git_object *obj, Repository *repo) {
    Object *py_obj = NULL;
    switch (git_object_type(obj)) {
        case GIT_OBJ_COMMIT:
            py_obj = (Object*)CommitType.tp_alloc(&CommitType, 0);
            break;
        case GIT_OBJ_TREE:
            py_obj = (Object*)TreeType.tp_alloc(&TreeType, 0);
            break;
        case GIT_OBJ_BLOB:
            py_obj = (Object*)BlobType.tp_alloc(&BlobType, 0);
            break;
        case GIT_OBJ_TAG:
            py_obj = (Object*)ObjectType.tp_alloc(&ObjectType, 0);
            break;
        default:
            assert(0);
    }
    if (!py_obj)
        return NULL;

    py_obj->obj = obj;
    py_obj->repo = repo;
    Py_INCREF(repo);
    return py_obj;
}

static PyObject *
Repository_getitem(Repository *self, PyObject *value) {
    char *hex;
    git_oid oid;
    git_object *obj;
    Object *py_obj;

    hex = PyString_AsString(value);
    if (!hex)
        return NULL;
    if (git_oid_mkstr(&oid, hex) < 0) {
        PyErr_Format(PyExc_ValueError, "Invalid hex SHA \"%s\"", hex);
        return NULL;
    }

    obj = git_repository_lookup(self->repo, &oid, GIT_OBJ_ANY);
    if (!obj) {
        PyErr_Format(PyExc_RuntimeError, "Failed to look up hex SHA \"%s\"",
                     hex);
        return NULL;
    }

    py_obj = wrap_object(obj, self);
    if (!py_obj)
        return NULL;
    py_obj->own_obj = 0;
    return (PyObject*)py_obj;
}

static git_rawobj
repository_raw_read(git_repository *repo, const git_oid *oid) {
    git_odb *db;
    git_rawobj raw;

    db = git_repository_database(repo);
    if (git_odb_read(&raw, db, oid) < 0) {
        PyErr_SetString(PyExc_RuntimeError, "Unable to read object");
        return raw;
    }
    return raw;
}

static PyObject *
Repository_read(Repository *self, PyObject *py_hex) {
    char *hex;
    git_oid id;
    git_rawobj raw;
    PyObject *result;

    hex = PyString_AsString(py_hex);
    if (!hex) {
        PyErr_SetString(PyExc_TypeError, "Expected string for hex SHA");
        return NULL;
    }

    if (git_oid_mkstr(&id, hex) < 0) {
        PyErr_SetString(PyExc_ValueError, "Invalid hex SHA");
        return NULL;
    }

    raw = repository_raw_read(self->repo, &id);
    if (!raw.data) {
        PyErr_Format(PyExc_RuntimeError, "Failed to read hex SHA \"%s\"", hex);
        return NULL;
    }

    result = Py_BuildValue("(ns#)", raw.type, raw.data, raw.len);
    free(raw.data);
    return result;
}

static PyMethodDef Repository_methods[] = {
    {"read", (PyCFunction)Repository_read, METH_O,
     "Read raw object data from the repository."},
    {NULL, NULL, 0, NULL}
};

static PySequenceMethods Repository_as_sequence = {
    0,                               /* sq_length */
    0,                               /* sq_concat */
    0,                               /* sq_repeat */
    0,                               /* sq_item */
    0,                               /* sq_slice */
    0,                               /* sq_ass_item */
    0,                               /* sq_ass_slice */
    (objobjproc)Repository_contains, /* sq_contains */
};

static PyMappingMethods Repository_as_mapping = {
    0,                               /* mp_length */
    (binaryfunc)Repository_getitem,  /* mp_subscript */
    0,                               /* mp_ass_subscript */
};

static PyTypeObject RepositoryType = {
    PyObject_HEAD_INIT(NULL)
    0,                                         /* ob_size */
    "pygit2.Repository",                       /* tp_name */
    sizeof(Repository),                        /* tp_basicsize */
    0,                                         /* tp_itemsize */
    (destructor)Repository_dealloc,            /* tp_dealloc */
    0,                                         /* tp_print */
    0,                                         /* tp_getattr */
    0,                                         /* tp_setattr */
    0,                                         /* tp_compare */
    0,                                         /* tp_repr */
    0,                                         /* tp_as_number */
    &Repository_as_sequence,                   /* tp_as_sequence */
    &Repository_as_mapping,                    /* tp_as_mapping */
    0,                                         /* tp_hash  */
    0,                                         /* tp_call */
    0,                                         /* tp_str */
    0,                                         /* tp_getattro */
    0,                                         /* tp_setattro */
    0,                                         /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,  /* tp_flags */
    "Git repository",                          /* tp_doc */
    0,                                         /* tp_traverse */
    0,                                         /* tp_clear */
    0,                                         /* tp_richcompare */
    0,                                         /* tp_weaklistoffset */
    0,                                         /* tp_iter */
    0,                                         /* tp_iternext */
    Repository_methods,                        /* tp_methods */
    0,                                         /* tp_members */
    0,                                         /* tp_getset */
    0,                                         /* tp_base */
    0,                                         /* tp_dict */
    0,                                         /* tp_descr_get */
    0,                                         /* tp_descr_set */
    0,                                         /* tp_dictoffset */
    (initproc)Repository_init,                 /* tp_init */
    0,                                         /* tp_alloc */
    0,                                         /* tp_new */
};

static void
Object_dealloc(Object* self)
{
    if (self->own_obj)
        git_object_free(self->obj);
    Py_XDECREF(self->repo);
    self->ob_type->tp_free((PyObject*)self);
}

static PyObject *
Object_get_type(Object *self, void *closure) {
    return PyInt_FromLong(git_object_type(self->obj));
}

static PyObject *
Object_get_sha(Object *self, void *closure) {
    const git_oid *id;
    char hex[GIT_OID_HEXSZ];

    id = git_object_id(self->obj);
    if (!id)
        return Py_None;

    git_oid_fmt(hex, id);
    return PyString_FromStringAndSize(hex, GIT_OID_HEXSZ);
}

static PyObject *
Object_read_raw(Object *self) {
    const git_oid *id;
    git_rawobj raw;
    PyObject *result;

    id = git_object_id(self->obj);
    if (!id)
        return Py_None;  /* in-memory object */

    raw = repository_raw_read(git_object_owner(self->obj), id);
    if (!raw.data) {
        PyErr_SetString(PyExc_RuntimeError, "Failed to read object");
        return NULL;
    }

    result = PyString_FromStringAndSize(raw.data, raw.len);
    free(raw.data);
    return result;
}

static PyObject *
Object_write(Object *self) {
    if (git_object_write(self->obj) < 0) {
        PyErr_SetString(PyExc_RuntimeError, "Failed to write object to repo");
        return NULL;
    }
    return Py_None;
}

static PyGetSetDef Object_getseters[] = {
    {"type", (getter)Object_get_type, NULL, "type number", NULL},
    {"sha", (getter)Object_get_sha, NULL, "hex SHA", NULL},
    {NULL}
};

static PyMethodDef Object_methods[] = {
    {"read_raw", (PyCFunction)Object_read_raw, METH_NOARGS,
     "Read the raw contents of the object from the repo."},
    {"write", (PyCFunction)Object_write, METH_NOARGS,
     "Write the object to the repo, if changed."},
    {NULL}
};

static PyTypeObject ObjectType = {
    PyObject_HEAD_INIT(NULL)
    0,                                         /*ob_size*/
    "pygit2.Object",                           /*tp_name*/
    sizeof(Object),                            /*tp_basicsize*/
    0,                                         /*tp_itemsize*/
    (destructor)Object_dealloc,                /*tp_dealloc*/
    0,                                         /*tp_print*/
    0,                                         /*tp_getattr*/
    0,                                         /*tp_setattr*/
    0,                                         /*tp_compare*/
    0,                                         /*tp_repr*/
    0,                                         /*tp_as_number*/
    0,                                         /*tp_as_sequence*/
    0,                                         /*tp_as_mapping*/
    0,                                         /*tp_hash */
    0,                                         /*tp_call*/
    0,                                         /*tp_str*/
    0,                                         /*tp_getattro*/
    0,                                         /*tp_setattro*/
    0,                                         /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,  /*tp_flags*/
    "Object objects",                          /* tp_doc */
    0,                                         /* tp_traverse */
    0,                                         /* tp_clear */
    0,                                         /* tp_richcompare */
    0,                                         /* tp_weaklistoffset */
    0,                                         /* tp_iter */
    0,                                         /* tp_iternext */
    Object_methods,                            /* tp_methods */
    0,                                         /* tp_members */
    Object_getseters,                          /* tp_getset */
    0,                                         /* tp_base */
    0,                                         /* tp_dict */
    0,                                         /* tp_descr_get */
    0,                                         /* tp_descr_set */
    0,                                         /* tp_dictoffset */
    0,                                         /* tp_init */
    0,                                         /* tp_alloc */
    0,                                         /* tp_new */
};

static int
object_init_check(const char *type_name, PyObject *args, PyObject *kwds,
                  Repository **repo) {
    if (kwds) {
        PyErr_Format(PyExc_TypeError, "%s takes no keyword arugments",
                     type_name);
        return 0;
    }

    if (!PyArg_ParseTuple(args, "O", repo))
        return 0;

    if (!PyObject_TypeCheck(*repo, &RepositoryType)) {
        PyErr_SetString(PyExc_TypeError, "Expected Repository for repo");
        return 0;
    }
    return 1;
}

static int
Commit_init(Commit *py_commit, PyObject *args, PyObject *kwds) {
    Repository *repo = NULL;
    git_commit *commit;

    if (!object_init_check("Commit", args, kwds, &repo))
        return -1;

    commit = git_commit_new(repo->repo);
    if (!commit) {
        PyErr_SetNone(PyExc_MemoryError);
        return -1;
    }
    Py_INCREF(repo);
    py_commit->repo = repo;
    py_commit->own_obj = 1;
    py_commit->commit = commit;
    return 0;
}

static PyObject *
Commit_get_message_short(Commit *commit) {
    return PyString_FromString(git_commit_message_short(commit->commit));
}

static PyObject *
Commit_get_message(Commit *commit) {
    return PyString_FromString(git_commit_message(commit->commit));
}

static int
Commit_set_message(Commit *commit, PyObject *message) {
    if (!PyString_Check(message)) {
        PyErr_SetString(PyExc_TypeError, "Expected string for commit message.");
        return -1;
    }
    git_commit_set_message(commit->commit, PyString_AS_STRING(message));
    return 0;
}

static PyObject *
Commit_get_commit_time(Commit *commit) {
    return PyLong_FromLong(git_commit_time(commit->commit));
}

static PyObject *
Commit_get_committer(Commit *commit) {
    git_person *committer;
    committer = (git_person*)git_commit_committer(commit->commit);
    return Py_BuildValue("(ssl)", git_person_name(committer),
                         git_person_email(committer),
                         git_person_time(committer));
}

static int
Commit_set_committer(Commit *commit, PyObject *value) {
    char *name = NULL, *email = NULL;
    long long time;
    if (!PyArg_ParseTuple(value, "ssL", &name, &email, &time))
        return -1;
    git_commit_set_committer(commit->commit, name, email, time);
    return 0;
}

static PyObject *
Commit_get_author(Commit *commit) {
    git_person *author;
    author = (git_person*)git_commit_author(commit->commit);
    return Py_BuildValue("(ssl)", git_person_name(author),
                         git_person_email(author),
                         git_person_time(author));
}

static int
Commit_set_author(Commit *commit, PyObject *value) {
    char *name = NULL, *email = NULL;
    long long time;
    if (!PyArg_ParseTuple(value, "ssL", &name, &email, &time))
        return -1;
    git_commit_set_author(commit->commit, name, email, time);
    return 0;
}

static PyGetSetDef Commit_getseters[] = {
    {"message_short", (getter)Commit_get_message_short, NULL, "short message",
     NULL},
    {"message", (getter)Commit_get_message, (setter)Commit_set_message,
     "message", NULL},
    {"commit_time", (getter)Commit_get_commit_time, NULL, "commit time",
     NULL},
    {"committer", (getter)Commit_get_committer,
     (setter)Commit_set_committer, "committer", NULL},
    {"author", (getter)Commit_get_author,
     (setter)Commit_set_author, "author", NULL},
    {NULL}
};

static PyTypeObject CommitType = {
    PyObject_HEAD_INIT(NULL)
    0,                                         /*ob_size*/
    "pygit2.Commit",                           /*tp_name*/
    sizeof(Commit),                            /*tp_basicsize*/
    0,                                         /*tp_itemsize*/
    0,                                         /*tp_dealloc*/
    0,                                         /*tp_print*/
    0,                                         /*tp_getattr*/
    0,                                         /*tp_setattr*/
    0,                                         /*tp_compare*/
    0,                                         /*tp_repr*/
    0,                                         /*tp_as_number*/
    0,                                         /*tp_as_sequence*/
    0,                                         /*tp_as_mapping*/
    0,                                         /*tp_hash */
    0,                                         /*tp_call*/
    0,                                         /*tp_str*/
    0,                                         /*tp_getattro*/
    0,                                         /*tp_setattro*/
    0,                                         /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,  /*tp_flags*/
    "Commit objects",                          /* tp_doc */
    0,                                         /* tp_traverse */
    0,                                         /* tp_clear */
    0,                                         /* tp_richcompare */
    0,                                         /* tp_weaklistoffset */
    0,                                         /* tp_iter */
    0,                                         /* tp_iternext */
    0,                                         /* tp_methods */
    0,                                         /* tp_members */
    Commit_getseters,                          /* tp_getset */
    0,                                         /* tp_base */
    0,                                         /* tp_dict */
    0,                                         /* tp_descr_get */
    0,                                         /* tp_descr_set */
    0,                                         /* tp_dictoffset */
    (initproc)Commit_init,                     /* tp_init */
    0,                                         /* tp_alloc */
    0,                                         /* tp_new */
};

static void
TreeEntry_dealloc(TreeEntry *self) {
    Py_XDECREF(self->tree);
    self->ob_type->tp_free((PyObject *)self);
}

static PyObject *
TreeEntry_get_attributes(TreeEntry *self) {
    return PyInt_FromLong(git_tree_entry_attributes(self->entry));
}

static int
TreeEntry_set_attributes(TreeEntry *self, PyObject *value) {
    unsigned int attributes;
    attributes = PyInt_AsLong(value);
    if (PyErr_Occurred())
        return -1;
    git_tree_entry_set_attributes(self->entry, attributes);
    return 0;
}

static PyObject *
TreeEntry_get_name(TreeEntry *self) {
    return PyString_FromString(git_tree_entry_name(self->entry));
}

static int
TreeEntry_set_name(TreeEntry *self, PyObject *value) {
    char *name;
    name = PyString_AsString(value);
    if (!name)
        return -1;
    git_tree_entry_set_name(self->entry, name);
    return 0;
}

static PyObject *
TreeEntry_get_sha(TreeEntry *self) {
    char hex[GIT_OID_HEXSZ];
    git_oid_fmt(hex, git_tree_entry_id(self->entry));
    return PyString_FromStringAndSize(hex, GIT_OID_HEXSZ);
}

static int
TreeEntry_set_sha(TreeEntry *self, PyObject *value) {
    char *hex;
    git_oid oid;

    hex = PyString_AsString(value);
    if (!hex)
        return -1;
    if (git_oid_mkstr(&oid, hex) < 0) {
        PyErr_Format(PyExc_ValueError, "Invalid hex SHA \"%s\"", hex);
        return -1;
    }
    git_tree_entry_set_id(self->entry, &oid);
    return 0;
}

static PyObject *
TreeEntry_to_object(TreeEntry *self) {
    git_object *obj;
    char hex[GIT_OID_HEXSZ];
    PyObject *py_hex;

    obj = git_tree_entry_2object(self->entry);
    if (!obj) {
        git_oid_fmt(hex, git_tree_entry_id(self->entry));
        py_hex = PyString_FromStringAndSize(hex, GIT_OID_HEXSZ);
        PyErr_SetObject(PyExc_KeyError, py_hex);
        return NULL;
    }
    return (PyObject*)wrap_object(obj, self->tree->repo);
}

static PyGetSetDef TreeEntry_getseters[] = {
    {"attributes", (getter)TreeEntry_get_attributes,
     (setter)TreeEntry_set_attributes, "attributes", NULL},
    {"name", (getter)TreeEntry_get_name, (setter)TreeEntry_set_name, "name",
     NULL},
    {"sha", (getter)TreeEntry_get_sha, (setter)TreeEntry_set_sha, "sha", NULL},
    {NULL}
};

static PyMethodDef TreeEntry_methods[] = {
    {"to_object", (PyCFunction)TreeEntry_to_object, METH_NOARGS,
     "Look up the corresponding object in the repo."},
    {NULL, NULL, 0, NULL}
};

static PyTypeObject TreeEntryType = {
    PyObject_HEAD_INIT(NULL)
    0,                                         /*ob_size*/
    "pygit2.TreeEntry",                        /*tp_name*/
    sizeof(TreeEntry),                         /*tp_basicsize*/
    0,                                         /*tp_itemsize*/
    (destructor)TreeEntry_dealloc,             /*tp_dealloc*/
    0,                                         /*tp_print*/
    0,                                         /*tp_getattr*/
    0,                                         /*tp_setattr*/
    0,                                         /*tp_compare*/
    0,                                         /*tp_repr*/
    0,                                         /*tp_as_number*/
    0,                                         /*tp_as_sequence*/
    0,                                         /*tp_as_mapping*/
    0,                                         /*tp_hash */
    0,                                         /*tp_call*/
    0,                                         /*tp_str*/
    0,                                         /*tp_getattro*/
    0,                                         /*tp_setattro*/
    0,                                         /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,  /*tp_flags*/
    "TreeEntry objects",                       /* tp_doc */
    0,                                         /* tp_traverse */
    0,                                         /* tp_clear */
    0,                                         /* tp_richcompare */
    0,                                         /* tp_weaklistoffset */
    0,                                         /* tp_iter */
    0,                                         /* tp_iternext */
    TreeEntry_methods,                         /* tp_methods */
    0,                                         /* tp_members */
    TreeEntry_getseters,                       /* tp_getset */
    0,                                         /* tp_base */
    0,                                         /* tp_dict */
    0,                                         /* tp_descr_get */
    0,                                         /* tp_descr_set */
    0,                                         /* tp_dictoffset */
    0,                                         /* tp_init */
    0,                                         /* tp_alloc */
    0,                                         /* tp_new */
};

static int
Tree_init(Tree *py_tree, PyObject *args, PyObject *kwds) {
    Repository *repo = NULL;
    git_tree *tree;

    if (!object_init_check("Tree", args, kwds, &repo))
        return -1;

    tree = git_tree_new(repo->repo);
    if (!tree) {
        PyErr_SetNone(PyExc_MemoryError);
        return -1;
    }
    Py_INCREF(repo);
    py_tree->repo = repo;
    py_tree->own_obj = 1;
    py_tree->tree = tree;
    return 0;
}

static Py_ssize_t
Tree_len(Tree *self) {
    return (Py_ssize_t)git_tree_entrycount(self->tree);
}

static int
Tree_contains(Tree *self, PyObject *py_name) {
    char *name;
    name = PyString_AsString(py_name);
    return name && git_tree_entry_byname(self->tree, name) ? 1 : 0;
}

static TreeEntry *
wrap_tree_entry(git_tree_entry *entry, Tree *tree) {
    TreeEntry *py_entry = NULL;
    py_entry = (TreeEntry*)TreeEntryType.tp_alloc(&TreeEntryType, 0);
    if (!py_entry)
        return NULL;

    py_entry->entry = entry;
    py_entry->tree = tree;
    Py_INCREF(tree);
    return py_entry;
}

static TreeEntry *
Tree_getitem_by_name(Tree *self, PyObject *py_name) {
    char *name;
    git_tree_entry *entry;
    name = PyString_AS_STRING(py_name);
    entry = git_tree_entry_byname(self->tree, name);
    if (!entry) {
        PyErr_SetObject(PyExc_KeyError, py_name);
        return NULL;
    }
    return wrap_tree_entry(entry, self);
}

static int
Tree_fix_index(Tree *self, PyObject *py_index) {
    long index;
    size_t len;
    long slen;

    index = PyInt_AsLong(py_index);
    if (PyErr_Occurred())
        return -1;

    len = git_tree_entrycount(self->tree);
    slen = (long)len;
    if (index >= slen) {
        PyErr_SetObject(PyExc_IndexError, py_index);
        return -1;
    } else if (index < -slen) {
        PyErr_SetObject(PyExc_IndexError, py_index);
        return -1;
    }

    /* This function is called via mp_subscript, which doesn't do negative index
     * rewriting, so we have to do it manually. */
    if (index < 0)
        index = len + index;
    return (int)index;
}

static TreeEntry *
Tree_getitem_by_index(Tree *self, PyObject *py_index) {
    int index;
    git_tree_entry *entry;

    index = Tree_fix_index(self, py_index);
    if (PyErr_Occurred())
        return NULL;

    entry = git_tree_entry_byindex(self->tree, index);
    if (!entry) {
        PyErr_SetObject(PyExc_IndexError, py_index);
        return NULL;
    }
    return wrap_tree_entry(entry, self);
}

static TreeEntry *
Tree_getitem(Tree *self, PyObject *value) {
    if (PyString_Check(value)) {
        return Tree_getitem_by_name(self, value);
    } else if (PyInt_Check(value)) {
        return Tree_getitem_by_index(self, value);
    } else {
        PyErr_SetString(PyExc_TypeError, "Expected int or str for tree index.");
        return NULL;
    }
}

static int
Tree_delitem_by_name(Tree *self, PyObject *name) {
    int err;
    err = git_tree_remove_entry_byname(self->tree, PyString_AS_STRING(name));
    if (err < 0) {
        PyErr_SetObject(PyExc_KeyError, name);
        return -1;
    }
    return 0;
}

static int
Tree_delitem_by_index(Tree *self, PyObject *py_index) {
    int index, err;
    index = Tree_fix_index(self, py_index);
    if (PyErr_Occurred())
        return -1;
    err = git_tree_remove_entry_byindex(self->tree, index);
    if (err < 0) {
        PyErr_SetObject(PyExc_IndexError, py_index);
        return -1;
    }
    return 0;
}

static int
Tree_delitem(Tree *self, PyObject *name, PyObject *value) {
    /* TODO: This function is only used for deleting items. We may be able to
     * come up with some reasonable assignment semantics, but it's tricky
     * because git_tree_entry objects are owned by their containing tree. */
    if (value) {
        PyErr_SetString(PyExc_ValueError,
                        "Cannot set TreeEntry directly; use add_entry.");
        return -1;
    }

    if (PyString_Check(name)) {
        return Tree_delitem_by_name(self, name);
    } else if (PyInt_Check(name)) {
        return Tree_delitem_by_index(self, name);
    } else {
        PyErr_SetString(PyExc_TypeError, "Expected int or str for tree index.");
        return -1;
    }
}

static PyObject *
Tree_add_entry(Tree *self, PyObject *args) {
    char *hex, *name;
    int attributes;
    git_oid oid;

    if (!PyArg_ParseTuple(args, "ssi", &hex, &name, &attributes))
        return NULL;

    if (git_oid_mkstr(&oid, hex) < 0) {
        PyErr_Format(PyExc_ValueError, "Invalid hex SHA \"%s\"", hex);
        return NULL;
    }

    if (git_tree_add_entry(self->tree, &oid, name, attributes) < 0)
        return PyErr_NoMemory();
    return Py_None;
}

static PyMethodDef Tree_methods[] = {
    {"add_entry", (PyCFunction)Tree_add_entry, METH_VARARGS,
     "Add an entry to a Tree."},
    {NULL}
};

static PySequenceMethods Tree_as_sequence = {
    0,                          /* sq_length */
    0,                          /* sq_concat */
    0,                          /* sq_repeat */
    0,                          /* sq_item */
    0,                          /* sq_slice */
    0,                          /* sq_ass_item */
    0,                          /* sq_ass_slice */
    (objobjproc)Tree_contains,  /* sq_contains */
};

static PyMappingMethods Tree_as_mapping = {
    (lenfunc)Tree_len,            /* mp_length */
    (binaryfunc)Tree_getitem,     /* mp_subscript */
    (objobjargproc)Tree_delitem,  /* mp_ass_subscript */
};

static PyTypeObject TreeType = {
    PyObject_HEAD_INIT(NULL)
    0,                                         /*ob_size*/
    "pygit2.Tree",                             /*tp_name*/
    sizeof(Tree),                              /*tp_basicsize*/
    0,                                         /*tp_itemsize*/
    0,                                         /*tp_dealloc*/
    0,                                         /*tp_print*/
    0,                                         /*tp_getattr*/
    0,                                         /*tp_setattr*/
    0,                                         /*tp_compare*/
    0,                                         /*tp_repr*/
    0,                                         /*tp_as_number*/
    &Tree_as_sequence,                         /*tp_as_sequence*/
    &Tree_as_mapping,                          /*tp_as_mapping*/
    0,                                         /*tp_hash */
    0,                                         /*tp_call*/
    0,                                         /*tp_str*/
    0,                                         /*tp_getattro*/
    0,                                         /*tp_setattro*/
    0,                                         /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,  /*tp_flags*/
    "Tree objects",                            /* tp_doc */
    0,                                         /* tp_traverse */
    0,                                         /* tp_clear */
    0,                                         /* tp_richcompare */
    0,                                         /* tp_weaklistoffset */
    0,                                         /* tp_iter */
    0,                                         /* tp_iternext */
    Tree_methods,                              /* tp_methods */
    0,                                         /* tp_members */
    0,                                         /* tp_getset */
    0,                                         /* tp_base */
    0,                                         /* tp_dict */
    0,                                         /* tp_descr_get */
    0,                                         /* tp_descr_set */
    0,                                         /* tp_dictoffset */
    (initproc)Tree_init,                       /* tp_init */
    0,                                         /* tp_alloc */
    0,                                         /* tp_new */
};

static int
Blob_init(Blob *py_blob, PyObject *args, PyObject *kwds) {
    Repository *repo = NULL;
    git_object *blob;

    if (!object_init_check("blob", args, kwds, &repo))
        return -1;

    blob = git_object_new(repo->repo, GIT_OBJ_BLOB);
    if (!blob) {
        PyErr_SetNone(PyExc_MemoryError);
        return -1;
    }
    Py_INCREF(repo);
    py_blob->repo = repo;
    py_blob->own_obj = 1;
    py_blob->blob = blob;
    return 0;
}

/* TODO: libgit2 needs some way to set blob data. */
static PyGetSetDef Blob_getseters[] = {
    {"data", (getter)Object_read_raw, NULL, "raw data", NULL},
    {NULL}
};

static PyTypeObject BlobType = {
    PyObject_HEAD_INIT(NULL)
    0,                                         /*ob_size*/
    "pygit2.Blob",                             /*tp_name*/
    sizeof(Blob),                              /*tp_basicsize*/
    0,                                         /*tp_itemsize*/
    0,                                         /*tp_dealloc*/
    0,                                         /*tp_print*/
    0,                                         /*tp_getattr*/
    0,                                         /*tp_setattr*/
    0,                                         /*tp_compare*/
    0,                                         /*tp_repr*/
    0,                                         /*tp_as_number*/
    0,                                         /*tp_as_sequence*/
    0,                                         /*tp_as_mapping*/
    0,                                         /*tp_hash */
    0,                                         /*tp_call*/
    0,                                         /*tp_str*/
    0,                                         /*tp_getattro*/
    0,                                         /*tp_setattro*/
    0,                                         /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,  /*tp_flags*/
    "Blob objects",                            /* tp_doc */
    0,                                         /* tp_traverse */
    0,                                         /* tp_clear */
    0,                                         /* tp_richcompare */
    0,                                         /* tp_weaklistoffset */
    0,                                         /* tp_iter */
    0,                                         /* tp_iternext */
    0,                                         /* tp_methods */
    0,                                         /* tp_members */
    Blob_getseters,                            /* tp_getset */
    0,                                         /* tp_base */
    0,                                         /* tp_dict */
    0,                                         /* tp_descr_get */
    0,                                         /* tp_descr_set */
    0,                                         /* tp_dictoffset */
    (initproc)Blob_init,                       /* tp_init */
    0,                                         /* tp_alloc */
    0,                                         /* tp_new */
};

static PyMethodDef module_methods[] = {
    {NULL}
};

PyMODINIT_FUNC
initpygit2(void)
{
    PyObject* m;

    RepositoryType.tp_new = PyType_GenericNew;
    if (PyType_Ready(&RepositoryType) < 0)
        return;
    /* Do not set ObjectType.tp_new, to prevent creating Objects directly. */
    if (PyType_Ready(&ObjectType) < 0)
        return;
    CommitType.tp_base = &ObjectType;
    CommitType.tp_new = PyType_GenericNew;
    if (PyType_Ready(&CommitType) < 0)
        return;
    TreeEntryType.tp_base = &ObjectType;
    TreeEntryType.tp_new = PyType_GenericNew;
    if (PyType_Ready(&TreeEntryType) < 0)
        return;
    TreeType.tp_base = &ObjectType;
    TreeType.tp_new = PyType_GenericNew;
    if (PyType_Ready(&TreeType) < 0)
        return;
    BlobType.tp_base = &ObjectType;
    BlobType.tp_new = PyType_GenericNew;
    if (PyType_Ready(&BlobType) < 0)
        return;

    m = Py_InitModule3("pygit2", module_methods,
                       "Python bindings for libgit2.");

    if (m == NULL)
      return;

    Py_INCREF(&RepositoryType);
    PyModule_AddObject(m, "Repository", (PyObject *)&RepositoryType);

    Py_INCREF(&ObjectType);
    PyModule_AddObject(m, "Object", (PyObject *)&ObjectType);

    Py_INCREF(&CommitType);
    PyModule_AddObject(m, "Commit", (PyObject *)&CommitType);

    Py_INCREF(&TreeEntryType);
    PyModule_AddObject(m, "TreeEntry", (PyObject *)&TreeEntryType);

    Py_INCREF(&TreeType);
    PyModule_AddObject(m, "Tree", (PyObject *)&TreeType);

    Py_INCREF(&BlobType);
    PyModule_AddObject(m, "Blob", (PyObject *)&BlobType);

    PyModule_AddIntConstant(m, "GIT_OBJ_ANY", GIT_OBJ_ANY);
    PyModule_AddIntConstant(m, "GIT_OBJ_COMMIT", GIT_OBJ_COMMIT);
    PyModule_AddIntConstant(m, "GIT_OBJ_TREE", GIT_OBJ_TREE);
    PyModule_AddIntConstant(m, "GIT_OBJ_BLOB", GIT_OBJ_BLOB);
    PyModule_AddIntConstant(m, "GIT_OBJ_TAG", GIT_OBJ_TAG);
}
