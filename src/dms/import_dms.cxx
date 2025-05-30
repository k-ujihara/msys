#include "dms.hxx"
#include "../analyze.hxx"
#include "../clone.hxx"
#include "../dms.hxx"
#include "../import.hxx"
#include "../override.hxx"
#include "../term_table.hxx"

#include <sstream>
#include <stdio.h>
#include <string.h>
#include <stdexcept>
#include <mutex>

#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>

using namespace desres::msys;

typedef std::set<String> KnownSet;

static std::mutex dms_reader_lock;

static bool is_pN(std::string const& _s) {
    const char* s = _s.c_str();
    if (s[0]!='p') return false;
    while (*(++s)) {
        if (*s<'0' || *s>'9') return false;
    }
    return true;
}

static void read(Reader r, int col, ValueRef val) {
    switch (val.type()) {
        case FloatType: 
            val=r.get_flt(col); 
            break;
        case IntType: 
            val=r.get_int(col); 
            break;
        default:
        case StringType: 
            val=r.get_str(col); 
            break;
    }
}

static IdList read_params( Reader r, ParamTablePtr p,
                           bool ignore_ids = true ) {
    int i,n = r.size();
    int idcol=-1;
    for (i=0; i<n; i++) {
        std::string prop = r.name(i);
        /* ignore id, assuming param ids are 0-based */
        if (ignore_ids && prop=="id") {
            idcol=i;
            continue;
        }
        p->addProp(prop, r.type(i));
    }
    IdList idmap;
    for (; r; r.next()) {
        Id param = p->addParam();
        int j=0;
        for (i=0; i<n; i++) {
            if (i==idcol) {
                Id id = r.get_int(i);
                while (idmap.size()<id) idmap.push_back(BadId);
                idmap.push_back(param);
                continue;
            }
            read(r,i,p->value(param,j++));
        }
    }
    return idmap;
}

static void read_table( Sqlite dms, 
                        System& sys, 
                        const std::string& category,
                        const std::string& table,
                        KnownSet& known ) {

    std::string term_table = table + "_term";
    std::string param_table = table + "_param";

    /* find the terms table */
    known.insert(term_table);
    known.insert(param_table);
    Reader r = dms.fetch(term_table);
    if (!r.size()) {
        term_table = table;
        known.insert(term_table);
        r = dms.fetch(term_table);
        if (!r.size()) {
            MSYS_FAIL(category << " table " << table << " not found");
            return;
        }
    }

    /* extract column data */
    int paramcol=-1;
    int paramAcol=-1;
    int paramBcol=-1;
    std::vector<int> cols;
    /* everything not an atom or param is an extra term property */
    typedef std::map<int,std::pair<String,ValueType> > ExtraMap;
    ExtraMap extra;
    for (int i=0; i<r.size(); i++) {
        std::string prop = r.name(i);
        if (is_pN(prop)) cols.push_back(i);
        else if (prop=="param") paramcol=i;
        else if (prop=="paramA") paramAcol=i;
        else if (prop=="paramB") paramBcol=i;
        else {
            extra[i]=std::make_pair( prop,r.type(i));
        }
    }
    const unsigned natoms = cols.size();

    /* create and fill a TermTable entry in the System */
    TermTablePtr terms = sys.addTable(table, natoms);
    terms->category = parse(category);

    /* If a param column was found, then we expect there to be a parameter
     * table with the usual name.  If paramA and paramB were found, then
     * the table is alchemical, and the name of the parameter table is found
     * by removing the leading "alchemical_" from the table name.
     * If none of the three were found, and the param_table table was not 
     * found, then there is no parameter table at all, so we create one 
     * using the "extra" columns as properties.  Anything else we call 
     * not well formatted. */
    IdList idmap;
    ParamTablePtr rp;
    bool separate_param_table = false;
    if (paramcol>=0 && paramAcol==-1 && paramBcol==-1) {
        Reader r = dms.fetch(param_table);
        if (!r.size()) MSYS_FAIL("Missing param table at " << param_table);
        idmap = read_params(r, terms->params());
        separate_param_table = true;
    } else if (paramcol==-1 && paramAcol>=0 && paramBcol>=0) {
        param_table = param_table.substr(11);
        Reader r = dms.fetch(param_table);
        if (!r.size()) MSYS_FAIL("Missing param table at " << param_table);
        rp = ParamTable::create();
        idmap = read_params(r, rp);
        separate_param_table = true;
        for (Id i=0; i<rp->propCount(); i++) {
            terms->params()->addProp(rp->propName(i)+"A", rp->propType(i));
            terms->params()->addProp(rp->propName(i)+"B", rp->propType(i));
        }
    } else if (paramcol==-1 && paramAcol==-1 && paramBcol==-1) {
        Reader r = dms.fetch(param_table);
        if (r) {
            idmap = read_params(r, terms->params());
            separate_param_table = true;
        } else {
            for (ExtraMap::const_iterator i=extra.begin();i!=extra.end();++i) {
                terms->params()->addProp(i->second.first, i->second.second);
            }
        }
    } else {
        MSYS_FAIL("Term table " << term_table << " is misformatted.");
    }
    
    /* add extra properties */
    if (separate_param_table) {
        for (ExtraMap::const_iterator i=extra.begin(); i!=extra.end(); ++i) {
            terms->addTermProp(i->second.first, i->second.second);
        }
    }

    /* inject terms */
    IdList atoms(natoms);
    for (; r; r.next()) {
        /* read atoms */
        for (unsigned i=0; i<natoms; i++) {
            atoms[i] = r.get_int(cols[i]);
        }
        /* read param properties */
        Id param = BadId;
        if (rp) {
            /* alchemical case: each term gets its own param. */
            Id paramA = idmap.at(r.get_int(paramAcol));
            Id paramB = idmap.at(r.get_int(paramBcol));
            param = terms->params()->addParam();
            for (Id i=0; i<rp->propCount(); i++) {
                terms->params()->value(param,2*i  )=rp->value(paramA,i);
                terms->params()->value(param,2*i+1)=rp->value(paramB,i);
            }
        } else if (paramcol>=0) {
            /* regular non-alchemical case */
            param = idmap.at(r.get_int(paramcol));
        } else if (!separate_param_table) {
            /* params come from extra cols */
            param = terms->params()->addParam();
            Id j=0;
            for (ExtraMap::const_iterator i=extra.begin(); i!=extra.end(); ++i) {
                read(r,i->first,terms->params()->value(param,j++));
            }
        }
        Id term = terms->addTerm(atoms,param);
        /* read term properties */
        if (separate_param_table) {
            Id j=0;
            for (ExtraMap::const_iterator e=extra.begin(); e!=extra.end(); ++e) {
                read(r,e->first,terms->termPropValue(term, j++));
            }
        }
    }
}

static void read_metatables(Sqlite dms, System& sys, KnownSet& known) {
    static const char * categories[] = { 
        "bond", "constraint", "virtual", "polar" 
    };
    for (unsigned i=0; i<sizeof(categories)/sizeof(categories[0]); i++) {
        std::string category = categories[i];
        std::string metatable = category + "_term";
        known.insert(metatable);
        Reader r = dms.fetch(metatable);
        if (r) {
            int col=r.column("name");
            for (; r; r.next()) {
                std::string table = r.get_str(col);
                read_table( dms, sys, category, table, known );
            }
        }
    }
    std::string category = "nonbonded";
    std::string metatable = "nonbonded_table";
    known.insert(metatable);
    Reader r = dms.fetch(metatable);
    if (r) {
        int col=r.column("name");
        for (; r; r.next()) {
            std::string table = r.get_str(col);
            read_table( dms, sys, category, table, known );
        }
    }
}

static void 
read_nonbonded( Sqlite dms, System& sys, 
        IdList const& nbtypes, KnownSet& known ) {

    known.insert("nonbonded_param");
    Reader r = dms.fetch("nonbonded_param");
    if (!r) return;

    TermTablePtr terms = sys.addTable("nonbonded", 1);
    terms->category=NONBONDED;
    IdList idmap = read_params(r, terms->params());

    IdList atoms(1);
    unsigned i,n = nbtypes.size();
    for (i=0; i<n; i++) {
        atoms[0]=i;
        Id nb = nbtypes[i];
        if (!bad(nb)) {
            try {
                nb = idmap.at(nb);
            }
            catch (std::exception& e) {
                std::stringstream ss;
                ss << "ImportDMS: particle " << i << " has invalid nbtype " << nb;

                throw std::runtime_error(ss.str());
            }
        }
        terms->addTerm(atoms, nb);
    }
    known.insert("alchemical_particle");
    r = dms.fetch("alchemical_particle");
    if (r.size()) {
        int P0 = r.column("p0");
        int TYPEA = r.column("nbtypeA");
        int TYPEB = r.column("nbtypeB");
        /* chargeA gets treated specially: it overrides particle.charge */
        int CHARGEA = r.column("chargeA");

        if (P0<0 || TYPEA<0 || TYPEB<0) {
            throw std::runtime_error("malformed alchemical_particle table");
        }
        TermTablePtr alc = 
            sys.addTable("alchemical_nonbonded", 1, terms->params());
        alc->category = NONBONDED;
        std::vector<int> termprops;
        for (int i=0; i<r.size(); i++) {
            if (i==P0 || i==CHARGEA || i==TYPEA || i==TYPEB) continue;
            alc->addTermProp(r.name(i), r.type(i));
            termprops.push_back(i);
        }
        for (; r; r.next()) {
            int p0 = r.get_int(P0);
            atoms[0] = p0;
            if (!sys.hasAtom(atoms[0])) {
                MSYS_FAIL("alchemical_particle table has bad p0 '" << p0 << "'");
            }
            /* override nonbonded param */
            Id paramA = r.get_int(TYPEA);
            terms->setParam(atoms[0], idmap.at(paramA));

            /* add alchemical state */
            Id paramB = r.get_int(TYPEB);
            Id term = alc->addTerm(atoms, idmap.at(paramB));

            /* override charge */
            if (CHARGEA>=0) {
                atom_t& atm = sys.atom(atoms[0]);
                atm.charge = r.get_flt(CHARGEA);
            }

            /* add the rest as term properties */
            for (Id i=0; i<termprops.size(); i++) {
                read(r, termprops[i], alc->termPropValue(term, i));
            }
        }
    }
}

static void 
read_combined( Sqlite dms, System& sys, KnownSet& known ) {

    static const char* TABLE = "nonbonded_combined_param";
    known.insert(TABLE);
    Reader r = dms.fetch(TABLE);
    if (!r) return;
    TermTablePtr nb = sys.table("nonbonded");
    if (!nb) MSYS_FAIL("nonbonded_combined_param in dms file, but no nonbonded_param table");

    ParamTablePtr cb = ParamTable::create();
    read_params(r, cb, false);
    ParamTablePtr params = nb->overrides()->params();

    /* find param1 and param2 columns.  */
    Id param1col = BadId, param2col = BadId;
    IdList pcols;
    for (Id i=0; i<cb->propCount(); i++) {
        std::string prop = cb->propName(i);
        if (prop=="param1") param1col = i;
        else if (prop=="param2") param2col = i;
        else {
            params->addProp(prop, cb->propType(i));
            pcols.push_back(i);
        }
    }
    if (bad(param1col)) MSYS_FAIL("Missing param1 from " << TABLE);
    if (bad(param2col)) MSYS_FAIL("Missing param2 from " << TABLE);

    for (Id p=0; p<cb->paramCount(); p++) {
        params->addParam();
        for (Id i=0; i<pcols.size(); i++) {
            params->value(p,i) = cb->value(p,pcols[i]);
        }
        Id param1 = cb->value(p,param1col).asInt();
        Id param2 = cb->value(p,param2col).asInt();
        IdPair key(param1,param2);
        /* don't allow conflicting overrides on input. */
        Id oldp = nb->overrides()->get(key);
        if (!bad(oldp)) {
            if (params->compare(oldp,p)) {
                MSYS_FAIL("Conflicting " << TABLE << " entries for param1=" << param1 << " param2=" << param2);
            }
        }
        nb->overrides()->set(IdPair(param1,param2), p);
    }
}


static void
read_exclusions(Sqlite dms, System& sys, KnownSet& known) {
    known.insert("exclusion");
    /* some dms writers export exclusions with a term and param table. */
    known.insert("exclusion_term");
    known.insert("exclusion_param");

    Reader r = dms.fetch("exclusion");
    if (!r) return;

    TermTablePtr terms = sys.addTable("exclusion", 2);
    terms->reserve(dms.size("exclusion"));
    terms->category=EXCLUSION;
    IdList atoms(2);

    for (; r; r.next()) {
        atoms[0] = r.get_int(0);
        atoms[1] = r.get_int(1);
        terms->addTerm(atoms,-1);
    }
}

static void
read_nbinfo(Sqlite dms, System& sys, KnownSet& known) {
    known.insert("nonbonded_info");
    Reader r = dms.fetch("nonbonded_info");
    if (r) {
        int funct_col = r.column("vdw_funct");
        int rule_col = r.column( "vdw_rule");
        int esfunct_col = r.column("es_funct");
        if (funct_col>=0) {
            sys.nonbonded_info.vdw_funct = 
                r.get_str(funct_col);
        }
        if (rule_col>=0) {
            sys.nonbonded_info.vdw_rule =
                r.get_str(rule_col);
        }
        if (esfunct_col>=0) {
            sys.nonbonded_info.es_funct = 
                r.get_str(esfunct_col);
        }
    }
}

static void
read_cell(Sqlite dms, System& sys, KnownSet& known) {
    known.insert("global_cell");
    Reader r = dms.fetch("global_cell");
    if (r) {
        int col[3];
        col[0]=r.column("x");
        col[1]=r.column("y");
        col[2]=r.column("z");
        GlobalCell& cell = sys.global_cell;
        for (int i=0; i<3; i++) {
            for (int j=0; j<3; j++) {
                cell[i][j] = r.get_flt(col[j]);
            }
            r.next();
        }
        if (r) {
            throw std::runtime_error("global_cell table has too many rows");
        }
    }
}

static void read_extra( Sqlite dms, System& sys, const KnownSet& known) {
    Reader r = dms.fetch("sqlite_master");
    if (r) {
        int NAME = r.column("name");
        int TYPE = r.column("type");
        if (NAME<0 || TYPE<0) {
            throw std::runtime_error("malformed sqlite_master table");
        }
        for (; r; r.next()) {
            if (!strcmp(r.get_str(TYPE), "table")) {
                std::string extra = r.get_str(NAME);
                if (known.count(extra)) continue;
                Reader p = dms.fetch(extra);
                if (p) {
                    ParamTablePtr ptr = ParamTable::create();
                    sys.addAuxTable(extra, ptr);
                    read_params(p, ptr, false);
                    //printf("extra %s with %d params, %d props\n",
                        //extra.c_str(), ptr->paramCount(), ptr->propCount());
                }
            }
        }
    }
}

static void read_provenance( Sqlite dms, System& sys, KnownSet& known) {
    known.insert("provenance");
    Reader r = dms.fetch( "provenance");
    if (r) {
        int version = r.column( "version");
        int timestamp = r.column( "timestamp");
        int user = r.column( "user");
        int workdir = r.column( "workdir");
        int cmdline = r.column( "cmdline");
        int executable = r.column("executable");

        for (; r; r.next()) {
            Provenance p;
            if (version>=0)   p.version   = r.get_str( version);
            if (timestamp>=0) p.timestamp = r.get_str( timestamp);
            if (user>=0)      p.user      = r.get_str( user);
            if (workdir>=0)   p.workdir   = r.get_str( workdir);
            if (cmdline>=0)   p.cmdline   = r.get_str( cmdline);
            if (executable>=0) p.executable=r.get_str(executable);
            sys.addProvenance(p);
        }
    }
}

static void read_cts(Sqlite dms, System& sys, KnownSet& known) {
    known.insert("msys_ct");
    Reader r = dms.fetch("msys_ct", false);
    if (!r.size()) return;

    Id idcol = r.column("id");
    Id namecol = r.column("msys_name");
    for (; r; r.next()) {
        Id id = r.get_int(idcol);
        while (!sys.hasCt(id)) sys.addCt();
        auto& ct = sys.ct(id);
        ct.setName(r.get_str(namecol));
        for (Id i=0, n=r.size(); i<n; i++) {
            if (i==idcol || i==namecol) continue;
            auto type = r.numeric_type(i);
            std::string name = r.name(i);
            auto pos = name.find("_msys_converted_");   // see dms/export.cxx
            if (pos != std::string::npos) {
                name = name.substr(0, pos);
            }
            if (ct.has(name)) {
                // the _msys_convert trick was introduced in msys/3.7.313-316, but some DMS files
                // show up with a mixture of the old method and the method, so you can have, e.g.
                // both 'foobar_msys_converted_34' and 'foobar', which map to the same key.  which
                // to use?  failure is not an option.  Our basic tenet is not to throw away
                // information that the chemist might need.
                //
                // We have 4 cases, corresponding to the new and existing value being an empty string
                // or not.  If either value is empty string, the other value is preferred.  We can
                // use the non-empty value without issuing a warning.
                // Otherwise, if both are non-empty, we can't just drop one of them.  Convert them
                // both to a string, and concatenate with '|', issuing a warning.

                // empty string has lowest priority
                if (ct.type(name) == StringType && ct.value(name).c_str()[0] == '\0') {
                    ct.del(name);
                }  else if (type == StringType && r.get_str(i)[0] == '\0') {
                    // ignore this entry, keeping existing
                    continue;
                } else {
                    // concatenate old and new values
                    std::string newval;
                    switch (ct.type(name)) {
                        case IntType: newval = std::to_string(ct.value(name).asInt()); break;
                        case FloatType: newval = std::to_string(ct.value(name).asFloat()); break;
                        default:
                        case StringType: newval = ct.value(name).c_str(); break;
                    }
                    newval += "|";
                    newval += r.get_str(i);
                    MSYS_WARN("input system has both '" << r.name(i) << "' and '" << name << "' in ct " << id << "; constructed concatenated value '" << newval);
                    ct.del(name);
                    ct.add(name, StringType);
                    ct.value(name) = newval;
                    continue;
                }
            }
            ct.add(name, type);
            auto val = ct.value(name);
            switch (type) {
            case IntType: val =r.get_int(i); break;
            case FloatType: val = r.get_flt(i); break;
            default:
            case StringType: val = r.get_str(i); break;
            }
        }
    }
}

static void check_dms_version(Sqlite dms, KnownSet& known) {
    known.insert("dms_version");
    int my_major = msys_major_version();
    int my_minor = msys_minor_version();
    if (my_major == 0 && my_minor == 0) return;
    Reader r = dms.fetch("dms_version");
    if (!r.size()) return;
    int MAJOR = r.column("major");
    int MINOR = r.column("minor");
    if (MAJOR<0 || MINOR<0) {
        MSYS_FAIL("dms_version table is malformatted");
    }
    for (; r; r.next()) {
        int major = r.get_int(MAJOR);
        int minor = r.get_int(MINOR);
        if (major > msys_major_version() ||
            minor > msys_minor_version() ) {
        MSYS_FAIL("Application compiled with msys " << msys_version()
                << " is too old to read dms file with version "
                << major << "." << minor);
        }
    }
}

static SystemPtr import_dms( Sqlite dms, bool structure_only, 
                                         bool without_tables ) {

    SystemPtr h = System::create();
    System& sys = *h;

    KnownSet known;
    known.insert("particle");
    known.insert("bond");
    known.insert("msys_hash");
    
    check_dms_version(dms, known);

    read_cts(dms, sys, known);

    SystemImporter imp(h);

    IdList nbtypes;

    sys.atomReserve(dms.size("particle"));
    
    Reader r = dms.fetch("particle", false); /* no strict typing */
    if (!r.size()) MSYS_FAIL("Missing particle table");

    int SEGID = r.column("segid");
    int CHAIN = r.column("chain");
    int RESNAME = r.column("resname");
    int RESID = r.column("resid");
    int X = r.column("x");
    int Y = r.column("y");
    int Z = r.column("z");
    int VX = r.column("vx");
    int VY = r.column("vy");
    int VZ = r.column("vz");
    int MASS = r.column("mass");
    int ANUM = r.column("anum");
    int NAME = r.column("name");
    int NBTYPE = r.column("nbtype");
    int GID = r.column("id");
    int CHARGE = r.column("charge");
    int FORMAL = r.column("formal_charge");
    int INSERT = r.column("insertion");
    int CT = r.column("msys_ct");
    
    /* the rest of the columns are extra atom properties */
    std::set<int> handled;
    handled.insert(SEGID);
    handled.insert(CHAIN);
    handled.insert(RESNAME);
    handled.insert(RESID);
    handled.insert(X);
    handled.insert(Y);
    handled.insert(Z);
    handled.insert(VX);
    handled.insert(VY);
    handled.insert(VZ);
    handled.insert(MASS);
    handled.insert(ANUM);
    handled.insert(NAME);
    handled.insert(NBTYPE);
    handled.insert(GID);
    handled.insert(CHARGE);
    handled.insert(FORMAL);
    handled.insert(INSERT);
    handled.insert(CT);

    typedef std::map<int,ValueType> ExtraMap;
    ExtraMap extra;
    for (int i=0, n=r.size(); i<n; i++) {
        if (handled.count(i)) continue;
        extra[i]=r.type(i);
        sys.addAtomProp(r.name(i), extra[i]);
    }

    /* read the particle table */
    for (; r; r.next()) {
        int anum = r.get_int(ANUM);

        /* add atom */
        Id ct = CT<0 ? 0 : r.get_int(CT);
        const char * chainname = CHAIN<0 ? "" : r.get_str(CHAIN);
        const char * segid = SEGID<0 ? "" : r.get_str( SEGID);
        const char * resname = RESNAME<0 ? "" : r.get_str( RESNAME);
        const char * aname = NAME<0 ? "" : r.get_str( NAME);
        const char * insert = INSERT<0 ? "" : r.get_str(INSERT);
        int resnum = RESID<0 ? 0 : r.get_int( RESID);
        Id atmid = imp.addAtom(chainname, segid, resnum, resname, aname,insert,ct);

        /* add atom properties */
        atom_t& atm = sys.atomFAST(atmid);
        atm.x = r.get_flt( X);
        atm.y = r.get_flt( Y);
        atm.z = r.get_flt( Z);
        atm.vx = r.get_flt( VX);
        atm.vy = r.get_flt( VY);
        atm.vz = r.get_flt( VZ);
        atm.mass = r.get_flt( MASS);
        atm.atomic_number = anum;
        atm.charge = r.get_flt( CHARGE);
        atm.formal_charge = FORMAL<0 ? 0 : r.get_int(FORMAL);

        /* extra atom properties */
        Id propcol=0;
        for (ExtraMap::const_iterator iter=extra.begin();
                iter!=extra.end(); ++iter, ++propcol) {
            int col = iter->first;
            ValueType type = iter->second;
            ValueRef ref = sys.atomPropValue(atmid, propcol);
            if (type==IntType) 
                ref=r.get_int(col);
            else if (type==FloatType)
                ref=r.get_flt(col);
            else
                ref=r.get_str(col);
        }
        nbtypes.push_back(NBTYPE>=0 ? r.get_int(NBTYPE) : BadId);
    }

    sys.bondReserve(dms.size("bond"));

    r = dms.fetch("bond", false); /* no strict typing */
    if (r) {
        int p0=r.column("p0");
        int p1=r.column("p1");
        int o = r.column("order");

        /* read the bond table */
        for (; r; r.next()) {
            int ai = r.get_int( p0);
            int aj = r.get_int( p1);
            int order = o<0 ? 1 : r.get_int(o);
            Id id;
            try {
                id = sys.addBond(ai, aj);
            }
            catch (std::exception& e) {
                MSYS_FAIL("Failed adding bond (" << ai << ", " << aj << ")\n"
                        << e.what());
            }
            sys.bondFAST(id).order = order;
        }
    }

    read_cell(dms, sys, known);
    read_provenance(dms, sys, known);

    if (!without_tables) {
        read_metatables(dms, sys, known);
        read_nonbonded(dms, sys, nbtypes, known);
        read_combined(dms, sys, known);
        read_exclusions(dms, sys, known);
        read_nbinfo(dms, sys, known);
        read_extra(dms, sys, known);
    }

    if (structure_only) {
        // clone the non-pseudos if any pseudos were loaded.
        IdList ids;
        const Id n=sys.maxAtomId();
        ids.reserve(n);
        for (Id i=0, n=sys.maxAtomId(); i<n; i++) {
            if (sys.atomFAST(i).atomic_number>0) {
                ids.push_back(i);
            }
        }
        if (ids.size()<n) {
            h = Clone(h, ids);
            /* careful - at this point, sys points to freed memory! */
        }
    }

    Analyze(h);
    return h;
}

namespace {

    class iterator : public LoadIterator {
        // iteration over concatenated dms files

        int fd = 0;
        size_t offset = 0;
        const bool structure_only;
        const bool without_tables;

    public:
        iterator(std::string const& path, bool structure_only,
                                          bool without_tables)
        : structure_only(structure_only),
          without_tables(without_tables)
        {
            fd = ::open(path.data(), O_RDONLY);
            if (fd < 0) {
                MSYS_FAIL("Opening DMS file '" << path << "' for reading: "
                        << strerror(errno));
            }

        }

        void close() {
            if (fd > 0) {
                ::close(fd);
                fd = 0;
            }
        }

        ~iterator() {
            close();
        }

        SystemPtr next();
    };
}

SystemPtr iterator::next() {
    if (fd <= 0) return nullptr;
    // The first 100 bytes of an sqlite file correspond to the database header.
    char header[100];
    if (pread(fd, header, sizeof(header), offset) != sizeof(header)) {
        // calling this end of file
        close();
        return nullptr;
    }
    // validate the header string
    if (strcmp(header, "SQLite format 3")) {
        MSYS_FAIL("File has incorrect magic header string");
    }

    // get the page size: 2 bytes, big endian.
    // "The database page size in bytes. Must be a power of two between 512
    // and 32768 inclusive, or the value 1 representing a page size of 65536."
    uint32_t page_size =
        ((uint16_t)header[16] << 8) +
        ((uint16_t)header[17]     );
    if (page_size == 1) page_size = 65536;

    // get the number of pages.  DMS files written with older versions of
    // msys/sqlite will have zero here.  If so, we can't determine where the
    // db ends, so just bail rather than hope that this is the last entry.
    uint32_t page_count = 
        ((uint32_t)header[28] << 24) +
        ((uint32_t)header[29] << 16) +
        ((uint32_t)header[30] <<  8) +
        ((uint32_t)header[31]      );

    if (page_count == 0) {
        close();
        MSYS_FAIL("Entry at offset " << offset << " has zero page_count, possibly because it was written with an older version of msys");
    }
    uint64_t dbsize = page_size * page_count;

    // mmap the file
    void* ptr = mmap(0, dbsize, PROT_READ, MAP_PRIVATE, fd, offset);
    if (ptr == MAP_FAILED) {
        close();
        MSYS_FAIL("Mapping DMS entry at " << offset << " size " << dbsize
                << ": " << strerror(errno));
    }

    // read the db and import the system
    SystemPtr mol;
    try {
        std::lock_guard<std::mutex> lock(dms_reader_lock);
        Sqlite dms = Sqlite::read_bytes(ptr, dbsize);
        mol = import_dms(dms, structure_only, without_tables);
    }
    catch (std::exception& e) {
        munmap(ptr, dbsize);
        throw;
    }

    munmap(ptr, dbsize);
    offset += dbsize;

    if (mol->ctCount()) {
        mol->name = mol->ct(0).name();
    }
    return mol;
}

SystemPtr desres::msys::ImportDMS(const std::string& path, 
                                  bool structure_only,
                                  bool without_tables) {
    SystemPtr sys;
    try {
        std::lock_guard<std::mutex> lock(dms_reader_lock);
        Sqlite dms = Sqlite::read(path);
        sys = import_dms(dms, structure_only, without_tables);
    }
    catch (std::exception& e) {
        std::stringstream ss;
        ss << "Error opening dms file at '" << path << "': " << e.what();
        throw std::runtime_error(ss.str());
    }
    sys->name = path;
    return sys;
}

SystemPtr desres::msys::ImportDMSFromBytes( const char* bytes, int64_t len,
                                            bool structure_only,
                                            bool without_tables ) {
    SystemPtr sys;
    try {
        std::lock_guard<std::mutex> lock(dms_reader_lock);
        Sqlite dms = Sqlite::read_bytes(bytes, len);
        sys = import_dms(dms, structure_only, without_tables);
    }
    catch (std::exception& e) {
        std::stringstream ss;
        ss << "Error reading DMS byte stream: " << e.what();
        throw std::runtime_error(ss.str());
    }
    return sys;
}

static void no_close(sqlite3*) {}

SystemPtr desres::msys::sqlite::ImportDMS(sqlite3* db,
                                  bool structure_only,
                                  bool without_tables) {
    std::lock_guard<std::mutex> lock(dms_reader_lock);
    Sqlite dms(std::shared_ptr<sqlite3>(db,no_close));
    return import_dms(dms, structure_only, without_tables);
}

LoadIteratorPtr desres::msys::DMSIterator(std::string const& path,
                                          bool structure_only,
                                          bool without_tables) {
    return std::make_shared<iterator>(path, structure_only, without_tables);
}

