/*
Copyright (c) 2009 Sony Pictures Imageworks, et al.
All Rights Reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:
* Redistributions of source code must retain the above copyright
  notice, this list of conditions and the following disclaimer.
* Redistributions in binary form must reproduce the above copyright
  notice, this list of conditions and the following disclaimer in the
  documentation and/or other materials provided with the distribution.
* Neither the name of Sony Pictures Imageworks nor the names of its
  contributors may be used to endorse or promote products derived from
  this software without specific prior written permission.
THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifndef OSLEXEC_PVT_H
#define OSLEXEC_PVT_H

#include <string>
#include <vector>
#include <map>

#include "OpenImageIO/thread.h"

#include "oslexec.h"
#include "oslclosure.h"
#include "osl_pvt.h"
using namespace OSL;
using namespace OSL::pvt;


#ifdef OSL_NAMESPACE
namespace OSL_NAMESPACE {
#endif

namespace OSL {
namespace pvt {


// forward definitions
class ShadingSystemImpl;
class ShadingContext;
class ShadingExecution;



/// Like an int (of type T), but also internally keeps track of the 
/// maximum value is has held, and the total "requested" deltas.
template<typename T>
class PeakCounter
{
public:
    typedef T value_t;
    PeakCounter () : m_current(0), m_requested(0), m_peak(0) { }
    /// Reset all counts to zero.
    ///
    void clear () {
        m_current = 0;  m_requested = 0;  m_peak = 0;
    }
    /// Return the current value.
    ///
    value_t operator() () const { return m_current; }

    /// Return the current value.
    ///
    value_t current (void) const { return m_current; }
    /// Return the sum of all requests.
    ///
    value_t requested (void) const { return m_requested; }
    /// Return the peak value we saw.
    ///
    value_t peak (void) const { return m_peak; }

    /// Reassign the current value, adjust peak and requested as necessary.
    ///
    const value_t operator= (value_t newval) {
        value_t cur = m_current;
        if (newval > cur)
            m_requested += (cur-newval);
        m_current = newval;
        if (newval > m_peak)
            m_peak = newval;
        return m_current;
    }
    /// Add to current value, adjust peak and requested as necessary.
    ///
    const value_t operator+= (value_t sz) {
        m_requested += sz;
        m_current += sz;
        if (m_current > m_peak)
            m_peak = m_current;
        return m_current;
    }
    /// Add to current value, adjust peak and requested as necessary.
    ///
    const value_t operator-= (value_t sz) {
        m_current -= sz;
        return m_current;
    }
    const value_t operator++ ()    { *this += 1;  return m_current; }
    const value_t operator++ (int) { *this += 1;  return m_current-1; }
    const value_t operator-- ()    { *this -= 1;  return m_current; }
    const value_t operator-- (int) { *this -= 1;  return m_current+1; }

    friend std::ostream & operator<< (std::ostream &out, const PeakCounter &p)
    {
        out << p.requested() << " requested, " << p.peak() << " peak, "
            << p.current() << " current";
        return out;
    }
private:
    value_t m_current, m_requested, m_peak;
};



/// Reference to a shader Parameter from the user/app.  Use local
/// storage for simple scalar types (a single int, float, or string).
class ParamRef {
public:
    ParamRef (ustring name, TypeDesc type, const void *data)
        : m_name(name), m_type(type), m_data(data), m_is_local(false)
    {
        if (type == TypeDesc::TypeInt) {
            m_data = &m_local_data.i;
            m_local_data.i = *(const int *)data;
            m_is_local = true;
        } else if (type == TypeDesc::TypeFloat) {
            m_data = &m_local_data.f;
            m_local_data.f = *(const float *)data;
            m_is_local = true;
        } else if (type == TypeDesc::TypeString) {
            m_data = &m_local_data.s;
            m_local_data.s = ustring(*(const char **)data).c_str();
            m_is_local = true;
        }
    }
    ~ParamRef () { }
    ustring name () const { return m_name; }
    TypeDesc type () const { return m_type; }
    const void *data () const { return m_data; }
private:
    ustring m_name;         ///< Parameter name
    TypeDesc m_type;        ///< Parameter type
    const void *m_data;     ///< Pointer to data -- we are not the owner!
    bool m_is_local;        ///< Do we use local storage?
    union {
        int i;
        float f;
        const char *s;
    } m_local_data;         ///< Local storage for small simple types
};



/// ShaderMaster is the full internal representation of a complete
/// shader that would be a .oso file on disk: symbols, instructions,
/// arguments, you name it.  A master copy is shared by all the 
/// individual instances of the shader.
class ShaderMaster : public RefCnt {
public:
    typedef intrusive_ptr<ShaderMaster> ref;
    ShaderMaster (ShadingSystemImpl &shadingsys) : m_shadingsys(shadingsys) { }
    ~ShaderMaster () { }

    std::string print ();  // Debugging

    /// Return a pointer to the shading system for this master.
    ///
    ShadingSystemImpl & shadingsys () const { return m_shadingsys; }

    /// Run through the symbols and set up various things we can know
    /// with just the master: the size (including padding), and their
    /// data pointers if they are constants or params (to the defaults).
    /// As a side effect, also set this->m_firstparam/m_lastparam.
    void resolve_syms ();

    /// Run through the code, find an implementation for each op, do
    /// other housekeeping related to the code.
    void resolve_ops ();

    /// Find the named symbol, return its index in the symbol array, or
    /// -1 if not found.
    int findsymbol (ustring name) const;

    /// Find the named parameter, return its index in the symbol array, or
    /// -1 if not found.
    int findparam (ustring name) const;

    /// Return a pointer to the symbol (specified by integer index),
    /// or NULL (if index was -1, as returned by 'findsymbol').
    Symbol *symbol (int index) { return index >= 0 ? &m_symbols[index] : NULL; }

    /// Return the name of the shader.
    ///
    const std::string &shadername () const { return m_shadername; }

private:
    ShadingSystemImpl &m_shadingsys;    ///< Back-ptr to the shading system
    ShaderType m_shadertype;            ///< Type of shader
    std::string m_shadername;           ///< Shader name
    std::string m_osofilename;          ///< Full path of oso file
    OpcodeVec m_ops;                    ///< Actual code instructions
    std::vector<int> m_args;            ///< Arguments for all the ops
    // Need the code offsets for each code block
    SymbolVec m_symbols;                ///< Symbols used by the shader
    std::vector<int> m_idefaults;       ///< int default param values
    std::vector<float> m_fdefaults;     ///< float default param values
    std::vector<ustring> m_sdefaults;   ///< string default param values
    std::vector<int> m_iconsts;         ///< int constant values
    std::vector<float> m_fconsts;       ///< float constant values
    std::vector<ustring> m_sconsts;     ///< string constant values
    int m_firstparam, m_lastparam;      ///< Subset of symbols that are params
    friend class OSOReaderToMaster;
    friend class ShaderInstance;
    friend class ShadingExecution;
};



/// Describe one end of a parameter connetion: the parameter number, and
/// optinally an array index and/or channel number within that parameter.
struct ConnectedParam {
    int param;            ///< Parameter number (in the symbol table)
    int arrayindex;       ///< Array index (-1 for not an index)
    int channel;          ///< Channel number (-1 for no channel selection)
    int offset;           ///< Offset into the data of the element/channel
    TypeSpec type;        ///< Type of data being connected

    ConnectedParam () : param(-1), arrayindex(-1), channel(-1), offset(0) { }

    bool valid () const { return (param >= 0); }
};



/// Describe a parameter connection to an earlier layer.
///
struct Connection {
    int srclayer;          ///< Layer (within our group) of the source
    ConnectedParam src;    ///< Which source parameter (or part thereof)
    ConnectedParam dst;    ///< Which destination parameter (or part thereof)

    Connection (int srclay, const ConnectedParam &srccon,
                const ConnectedParam &dstcon)
        : srclayer (srclay), src (srccon), dst (dstcon)
    { }
};



/// ShaderInstance is a particular instance of a shader, with its own
/// set of parameter values, coordinate transform, and connections to
/// other instances within the same shader group.
class ShaderInstance {
public:
    typedef ShaderInstanceRef ref;
    ShaderInstance (ShaderMaster::ref master, const char *layername="");
    ~ShaderInstance () { }

    /// Return the layer name of this instance
    ///
    ustring layername () const { return m_layername; }

    /// Return a pointer to the master for this instance.
    ///
    ShaderMaster *master () const { return m_master.get(); }

    /// Return a reference to the shading system for this instance.
    ///
    ShadingSystemImpl & shadingsys () const { return m_master->shadingsys(); }

    /// Apply pending parameters
    /// 
    void parameters (const std::vector<ParamRef> &params);

    /// How much heap space this instance needs per point being shaded?
    /// As a side effect, set the dataoffset for each symbol (as if there
    /// was just one point being shaded).
    size_t heapsize ();

    /// Return a pointer to the symbol (specified by integer index),
    /// or NULL (if index was -1, as returned by 'findsymbol').
    Symbol *symbol (int index) { return index >= 0 ? &m_symbols[index] : NULL; }

    /// How many closures does this group use, not counting globals.
    ///
    size_t numclosures ();

    /// Estimate how much to round the required heap size up if npoints
    /// is odd, to account for getting the desired alignment for each
    /// symbol.
    size_t heapround ();

    /// Add a connection
    ///
    void add_connection (int srclayer, const ConnectedParam &srccon,
                         const ConnectedParam &dstcon) {
        m_connections.push_back (Connection (srclayer, srccon, dstcon));
    }

    /// How many connections to earlier layers do we have?
    ///
    int nconnections () const { return (int) m_connections.size (); }

    /// Return a reference to the i-th connection to an earlier layer.
    ///
    const Connection & connection (int i) const { return m_connections[i]; }

private:
    bool heap_size_calculated () const { return m_heapsize >= 0; }
    void calc_heap_size ();

    ShaderMaster::ref m_master;         ///< Reference to the master
    SymbolVec m_symbols;                ///< Symbols used by the instance
    ustring m_layername;                ///< Name of this layer
    std::vector<int> m_iparams;         ///< int param values
    std::vector<float> m_fparams;       ///< float param values
    std::vector<ustring> m_sparams;     ///< string param values
    int m_heapsize;                     ///< Heap space needed per point
    int m_heapround;                    ///< Heap padding for odd npoints
    int m_numclosures;                  ///< Number of non-global closures
    std::vector<Connection> m_connections; ///< Connected input params

    friend class ShadingExecution;
};



/// A ShaderGroup consists of one or more layers (each of which is a
/// ShaderInstance), and the connections among them.
class ShaderGroup {
public:
    ShaderGroup () { }
    ~ShaderGroup () { }

    /// Clear the layers
    ///
    void clear () { m_layers.clear (); }

    /// Append a new shader instance on to the end of this group
    ///
    void append (ShaderInstanceRef newlayer) {
        m_layers.push_back (newlayer);
    }

    /// How many layers are in this group?
    ///
    int nlayers () const { return (int) m_layers.size(); }

    /// Array indexing returns the i-th layer of the group
    ///
    ShaderInstance * operator[] (int i) const { return m_layers[i].get(); }

private:
    std::vector<ShaderInstanceRef> m_layers;
};



class ShadingSystemImpl : public ShadingSystem
{
public:
    ShadingSystemImpl (RendererServices *renderer=NULL,
                       TextureSystem *texturesystem=NULL,
                       ErrorHandler *err=NULL);
    virtual ~ShadingSystemImpl ();

    virtual bool attribute (const std::string &name, TypeDesc type, const void *val);
    virtual bool getattribute (const std::string &name, TypeDesc type, void *val);

    virtual bool Parameter (const char *name, TypeDesc t, const void *val);
    virtual bool Shader (const char *shaderusage,
                         const char *shadername=NULL,
                         const char *layername=NULL);
    virtual bool ShaderGroupBegin (void);
    virtual bool ShaderGroupEnd (void);
    virtual bool ConnectShaders (const char *srclayer, const char *srcparam,
                                 const char *dstlayer, const char *dstparam);
    virtual ShadingAttribStateRef state () const;
    virtual void clear_state ();

//    virtual void RunShaders (ShadingAttribStateRef &attribstate,
//                             ShaderUse use);

    /// Internal error reporting routine, with printf-like arguments.
    ///
    void error (const char *message, ...);

    /// Internal info printing routine, with printf-like arguments.
    ///
    void info (const char *message, ...);

    virtual std::string geterror () const;
    virtual std::string getstats (int level=1) const;

    ErrorHandler &errhandler () const { return *m_err; }

    ShaderMaster::ref loadshader (const char *name);

    /// Return a "blank" ShadingContext that we can use.
    ///
    shared_ptr<ShadingContext> get_context ();

    void operator delete (void *todel) { ::delete ((char *)todel); }

    /// Return the precomputed heap offset of the named global, or -1 if
    /// it's not precomputed.
    int global_heap_offset (ustring name);

    /// Is the shading system in debug mode?
    ///
    bool debug () const { return m_debug; }

    /// Return a pointer to the renderer services object.
    ///
    RendererServices *renderer () const { return m_renderer; }

    /// Return a pointer to the texture system.
    ///
    TextureSystem *texturesys () const { return m_texturesys; }

    /// Round up to an 8-byte boundary. 
    /// FIXME -- do we want to round up to 16-byte boundaries for 
    /// hardware SIMD reasons?
    size_t align_padding (size_t x, size_t alignment = 8) {
        size_t excess = (x & (alignment-1));
        return excess ? (alignment - excess) : 0;
    }

    /// Round up to an 8-byte boundary. 
    /// FIXME -- do we want to round up to 16-byte boundaries for 
    /// hardware SIMD reasons?
    size_t align (size_t x, size_t alignment = 8) {
        return x + align_padding (x, alignment);
    }

private:
    void printstats () const;
    void init_global_heap_offsets ();

    /// Find the index of the named layer in the current shader group.
    /// If found, return the index >= 0 and put a pointer to the instance
    /// in inst; if not found, return -1 and set inst to NULL.
    /// (This is a helper for ConnectShaders.)
    int find_named_layer_in_group (ustring layername, ShaderInstance * &inst);

    /// Turn a connectionname (such as "Kd" or "Cout[1]", etc.) into a
    /// ConnectedParam descriptor.  This routine is strictly a helper for
    /// ConnectShaders, and will issue error messages on its behalf.
    /// The return value will not be valid() if there is an error.
    ConnectedParam decode_connected_param (const char *connectionname,
                               const char *layername, ShaderInstance *inst);

    RendererServices *m_renderer;         ///< Renderer services
    TextureSystem *m_texturesys;          ///< Texture system
    ErrorHandler *m_err;                  ///< Error handler
    typedef std::map<ustring,ShaderMaster::ref> ShaderNameMap;
    ShaderNameMap m_shader_masters;       ///< name -> shader masters map
    int m_statslevel;                     ///< Statistics level
    bool m_debug;                         ///< Debugging output
    std::string m_searchpath;             ///< Shader search path
    std::vector<std::string> m_searchpath_dirs; ///< All searchpath dirs
    bool m_in_group;                      ///< Are we specifying a group?
    ShaderUse m_group_use;                ///< Use of group
    std::vector<ParamRef> m_pending_params; ///< Pending Parameter() values
    ShadingAttribStateRef m_curattrib;    ///< Current shading attribute state
    std::map<ustring,int> m_global_heap_offsets; ///< Heap offsets of globals
    size_t m_global_heap_total;           ///< Heap size for globals
    mutable mutex m_mutex;                ///< Thread safety
    /// Saved error string, per-thread
    ///
    mutable boost::thread_specific_ptr<std::string> m_errormessage;

    atomic_int m_stat_shaders_loaded;     ///< Stat: shaders loaded
    atomic_int m_stat_shaders_requested;  ///< Stat: shaders requested
    PeakCounter<int> m_stat_instances;    ///< Stat: instances
    PeakCounter<int> m_stat_contexts;     ///< Stat: shading contexts

    friend class ShadingContext;
};



class ShadingExecution;
typedef std::vector<ShadingExecution> ExecutionLayers;



/// The full context for executing a shader group.  This contains
/// ShadingExecution states for each shader
///
class ShadingContext {
public:
    ShadingContext (ShadingSystemImpl &shadingsys);
    ~ShadingContext ();

    /// Return a reference to the shading system for this context.
    ///
    ShadingSystemImpl & shadingsys () const { return m_shadingsys; }

    /// Return a pointer to our shader globals structure.
    ///
    ShaderGlobals *globals () const { return m_globals; }

    /// Set up this context for shading n points with the given shader
    /// attribute state and shader globals.  Resolve all the memory
    /// layout issues so that we're ready to execute().
    void bind (int n, ShadingAttribState &sas, ShaderGlobals &sg);

    /// Execute the shaders for the given use (for example,
    /// ShadUseSurface).  The context must already be bound.  If
    /// runflags are not supplied, they will be auto-generated with all
    /// points turned on.
    void execute (ShaderUse use, Runflag *rf=NULL);

    /// Return the number of points being shaded.
    ///
    int npoints () const { return m_npoints; }

    /// Return the address of a particular offset into the heap.
    ///
    void *heapaddr (size_t offset) { return &m_heap[offset]; }

    /// Allot 'size' bytes in the heap for this context, return its starting
    /// offset into the heap.
    size_t heap_allot (size_t size) {
        size_t cur = m_heap_allotted;
        m_heap_allotted += shadingsys().align (size);
        DASSERT (m_heap_allotted <= m_heap.size());
        return cur;
    }

    /// Allot whatever 'sym' needs on the heap and set up its offsets
    /// and adresses.  If 'varying' is true, set the symbol up to be
    /// initially varying.  Return the address in the heap where the
    /// memory for this symbol starts.
    void * heap_allot (Symbol &sym, bool varying = false) {
        size_t cur = heap_allot (sym.derivsize() * m_npoints);
        sym.dataoffset (cur);
        void *addr = heapaddr (cur);
        sym.data (addr);
        sym.step (varying ? sym.derivsize() : 0);
        return addr;
    }

    /// Allot 'n' closures in the closure area, and allot 'n' closure
    /// pointers in the heap (pointed to the allotted closures).  Return
    /// the starting offset into the heap of the pointers.
    size_t closure_allot (size_t n) {
        size_t curheap = heap_allot (n * sizeof (ClosureColor *));
        ClosureColor **ptrs = (ClosureColor **) heapaddr (curheap);
        for (size_t i = 0;  i < n;  ++i)
            ptrs[i] = &m_closures[m_closures_allotted++];
        return curheap;
    }

    /// Find the named symbol in the (already-executed!) stack of
    /// ShadingExecution's of the given use, with priority given to
    /// later laters over earlier layers (if they name the same symbol).
    /// Return NULL if no such symbol is found.
    Symbol * symbol (ShaderUse use, ustring name);

    ExecutionLayers &execlayer (ShaderUse use) { return m_exec[(int)use]; }

private:
    ShadingSystemImpl &m_shadingsys;    ///< Backpointer to shadingsys
    ShadingAttribState *m_attribs;      ///< Ptr to shading attrib state
    ShaderGlobals *m_globals;           ///< Ptr to shader globals
    std::vector<char> m_heap;           ///< Heap memory
    size_t m_heap_allotted;             ///< Heap memory allotted
    std::vector<ClosureColor> m_closures; ///< Closure memory
    size_t m_closures_allotted;         ///< Closure memory allotted
    ExecutionLayers m_exec[ShadUseLast];///< Execution layers for the group
    int m_npoints;                      ///< Number of points being shaded
    int m_nlights;                      ///< Number of lights
    int m_curlight;                     ///< Current light index
    int m_curuse;                       ///< Current use that we're running
    int m_nlayers[ShadUseLast];         ///< Number of layers for each use
    friend class ShadingExecution;
};



/// The state and machinery necessary to execute a single shader (node).
///
class ShadingExecution {
public:
    ShadingExecution ();
    ~ShadingExecution ();

    /// Bind an arena to prepare to run the shader.
    ///
    void bind (ShadingContext *context, ShaderUse use, int layerindex,
               ShaderInstance *instance);

    /// Execute the shader with the supplied runflags.
    ///
    void run (Runflag *rf=NULL);

    /// Execute the shader with the current runflags, over the range of
    /// ops denoted by [beginop, endop).
    void run (int beginop, int endop);

    /// Get a reference to the symbol with the given index.
    ///
    Symbol &sym (int index) { return m_symbols[index]; }

    /// Return the current instruction pointer index.
    ///
    int ip () const { return m_ip; }

    /// Set the instruction pointer index -- JUMP!
    ///
    void ip (int target) { m_ip = target; }

    /// Return a reference to the current op (pointed to by the instruction
    /// pointer).
    Opcode & op () const { return m_master->m_ops[m_ip]; }

    /// Adjust whether sym is uniform or varying, depending on what is
    /// about to be assigned to it.  In cases when sym is promoted from
    /// uniform to varying, 'preserve_value' determines if the old value
    /// should be preserved (and replicated to fill the new varying
    /// space) even if all shading points are turned on; it defaults to
    /// true (safe) but some shadeops may know that this isn't necessary
    /// and save the work. (If only a subset of points are enabled,
    /// e.g., in a conditional, the values will be preserved for 'off'
    /// points even if preserve_value is false.)  The value must be
    /// preserved if there's a chance that any symbols being written to
    /// are also symbols being read from in the same op.
    void adjust_varying (Symbol &sym, bool varying_assignment,
                         bool preserve_value = true);

    /// Set the value of sym (and its derivs, if it has them) to zero
    /// for all shading points that are turned on.
    void zero (Symbol &sym);

    /// Zero out the derivatives of sym for all shading points that are
    /// turned on.
    void zero_derivs (Symbol &sym);

    /// How many points are being shaded?
    ///
    int npoints () const { return m_npoints; }

    /// Are all shading points currently turned on for execution?
    ///
    bool all_points_on () const { return m_all_points_on; }

    // Set the runflags to rf[0..
    void new_runflags (Runflag *rf);

    /// Adjust the valid point range [m_beginpoint,m_endpoint) to
    /// newly-set runflags, but extending no farther than the begin/end
    /// range given, and set m_all_points_on to true iff all points are
    /// turned on.
    void new_runflag_range (int begin, int end);

    /// Set up a new run state, with the old one safely stored on the
    /// stack.
    void push_runflags (Runflag *runflags, int beginpoint, int endpoint);

    /// Restore the runflags to the state it was in before push_runflags().
    ///
    void pop_runflags ();

    bool debug () const { return m_debug; }

    /// Find the named symbol.  Return NULL if no such symbol is found.
    ///
    Symbol * symbol (ustring name) {
        int s = m_master->findsymbol (name);
        return s >= 0 ? &m_symbols[s] : NULL;
    }

    /// Format the value of sym using the printf-like format (taking a
    /// SINGLE value specifier), where 'whichpoint' gives the position
    /// in the set of shading points that we're concerned about.
    std::string format_symbol (const std::string &format, Symbol &sym,
                               int whichpoint);

    /// Turn the symbol into a string (for debugging).
    ///
    std::string printsymbolval (Symbol &sym);

    /// Get a pointer to the ShadingContext for this execution.
    ///
    ShadingContext *context () const { return m_context; }

    /// Get a pointer to the RendererServices for this execution.
    ///
    RendererServices *renderer () const { return m_renderer; }

    /// Get a pointer to the TextureSystem for this execution.
    ///
    TextureSystem *texturesys () const { return m_shadingsys->texturesys(); }

    /// Get the 4x4 matrix that transforms points from the named 'from'
    /// coordinate system to "common" space for the given shading point.
    void get_matrix (Matrix44 &result, ustring from, int whichpoint=0);

    /// Return the ShaderUse of this execution.
    ///
    ShaderUse shaderuse () const { return m_use; }

    /// Return the instance of this execution.
    ///
    ShaderInstance *instance () const { return m_instance; }

    /// Pass an error along to the ShadingSystem.
    ///
    void error (const char *message, ...);
    void warning (const char *message, ...);
    void info (const char *message, ...);
    void message (const char *message, ...);

    /// Generic type mismatch error
    ///
    void error_arg_types ();

private:
    /// Helper for bind(): run initialization code for parameters
    ///
    void bind_initialize_params (ShaderInstance *inst);

    /// Helper for bind(): take care of connections to earlier layers
    ///
    void bind_connections (ShaderInstance *inst);

    /// Helper for bind(): establish all connections to earlier layers
    /// for a given symbol.
    void bind_connection (ShaderInstance *inst, int symindex);

    ShaderUse m_use;              ///< Our shader use
    ShaderUse m_layerindex;       ///< Which layer are we?
    ShadingContext *m_context;    ///< Ptr to our shading context
    ShaderInstance *m_instance;   ///< Ptr to the shader instance
    ShaderMaster *m_master;       ///< Ptr to the instance's master
    ShadingSystemImpl *m_shadingsys; ///< Ptr to shading system
    RendererServices *m_renderer; ///< Ptr to renderer services
    int m_npoints;                ///< How many points are we running?
    bool m_bound;                 ///< Have we been bound?
    bool m_executed;              ///< Have we been executed?
    bool m_debug;                 ///< Debug mode
    SymbolVec m_symbols;          ///< Our own copy of the syms

    // A word about runflags and the runflag stack: the head of the
    // stack contains a copy of the CURRENT run state.  This is so that
    // certain operations can affect the whole set of runstates by
    // simply iterating over the stack entries, without treating the
    // current run state as a special case.
    struct Runstate {
        Runflag *runflags;
        int beginpoint, endpoint;
        bool allpointson;
        Runstate (Runflag *rf, int bp, int ep, bool all) 
            : runflags(rf), beginpoint(bp), endpoint(ep), allpointson(all) {}
    };
    typedef std::vector<Runstate> RunflagStack;

    Runflag *m_runflags;          ///< Current runflags
    int m_beginpoint;             ///< First point to shade
    int m_endpoint;               ///< One past last point to shade
    bool m_all_points_on;         ///< Are all points turned on?
    RunflagStack m_runflag_stack; ///< Stack of runflags
    int m_ip;                     ///< Instruction pointer

};



}; // namespace pvt





class ShadingAttribState
{
public:
    ShadingAttribState () : m_heapsize (-1 /*uninitialized*/),
                            m_heapround (-1), m_numclosures (-1) { }
    ~ShadingAttribState () { }

    /// Return a reference to the shader group for a particular use
    ///
    ShaderGroup & shadergroup (ShaderUse use) {
        return m_shaders[(int)use];
    }

    /// How much heap space does this group use per point being shaded?
    ///
    size_t heapsize ();

    /// How much heap space rounding might this group use?
    ///
    size_t heapround ();

    /// How many closures does this group use, not counting globals?
    ///
    size_t numclosures ();

private:
    bool heap_size_calculated () const { return m_heapsize >= 0; }
    void calc_heap_size ();

    OSL::pvt::ShaderGroup m_shaders[OSL::pvt::ShadUseLast];
    int m_heapsize;                  ///< Heap space needed per point
    int m_heapround;                 ///< Heap padding for odd npoints
    int m_numclosures;               ///< Number of non-global closures
};



namespace Strings {
    extern ustring camera, common, object, shader;
    extern ustring rgb, RGB, hsv, hsl, YIQ, xyz;
    extern ustring null, default_;
    extern ustring diffuse, phong, ward, transparent;
    extern ustring microfacet_beckmann, microfacet_ggx;
    extern ustring emission;
    extern ustring P, I, N, Ng, dPdu, dPdv, u, v, time, dtime, dPdtime;
    extern ustring Ci, Oi;
    extern ustring width, swidth, twidth, blur, sblur, tblur;
    extern ustring wrap, swrap, twrap, black, clamp, periodic, mirror;
    extern ustring firstchannel, fill, alpha;
}; // namespace Strings


}; // namespace OSL

#ifdef OSL_NAMESPACE
}; // end namespace OSL_NAMESPACE
using namespace OSL_NAMESPACE;
#endif

#endif /* OSLEXEC_PVT_H */
